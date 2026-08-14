// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "relay_health_check_internal.h"

#include "quicr/client.h"
#include "quicr/handlers/publish_track_handler.h"
#include "quicr/handlers/subscribe_track_handler.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace quicr;
using laps::relay_health::Options;

namespace {

    struct VerificationResult
    {
        bool matched{ false };
        std::string error;
    };

    template<typename Predicate>
    bool WaitFor(Predicate predicate,
                 std::chrono::milliseconds timeout,
                 std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10))
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) <
               timeout) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(poll_interval);
        }
        return predicate();
    }

    Bytes ToQuicrBytes(std::string_view value)
    {
        const auto bytes = laps::relay_health::ToBytes(value);
        return Bytes(bytes.begin(), bytes.end());
    }

    std::string DefaultTrackName()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return laps::relay_health::MakeDefaultTrackName(static_cast<std::uint64_t>(now));
    }

    class VerifyingSubscribeTrackHandler final : public SubscribeTrackHandler
    {
      public:
        static std::shared_ptr<VerifyingSubscribeTrackHandler> Create(
          const FullTrackName& full_track_name,
          Bytes expected_payload,
          std::shared_ptr<std::promise<VerificationResult>> result)
        {
            return std::shared_ptr<VerifyingSubscribeTrackHandler>(
              new VerifyingSubscribeTrackHandler(full_track_name, std::move(expected_payload), std::move(result)));
        }

        void ObjectReceived(const ObjectHeaders& object_headers,
                            BytesSpan data,
                            std::optional<messages::StreamHeaderProperties>) override
        {
            std::lock_guard lock(mutex_);
            if (completed_) {
                return;
            }
            completed_ = true;

            const Bytes received(data.begin(), data.end());
            if (received != expected_payload_) {
                std::ostringstream error;
                error << "payload mismatch for group " << object_headers.group_id << ", subgroup "
                      << object_headers.subgroup_id << ", object " << object_headers.object_id << ": expected "
                      << expected_payload_.size() << " bytes, received " << received.size() << " bytes";
                result_->set_value({ .matched = false, .error = error.str() });
                return;
            }

            result_->set_value({ .matched = true, .error = {} });
        }

      private:
        VerifyingSubscribeTrackHandler(const FullTrackName& full_track_name,
                                       Bytes expected_payload,
                                       std::shared_ptr<std::promise<VerificationResult>> result)
          : SubscribeTrackHandler(full_track_name, 3, std::nullopt)
          , expected_payload_(std::move(expected_payload))
          , result_(std::move(result))
        {
        }

        std::mutex mutex_;
        bool completed_{ false };
        Bytes expected_payload_;
        std::shared_ptr<std::promise<VerificationResult>> result_;
    };

    std::shared_ptr<Client> MakeClient(const std::string& endpoint_id, const Options& options)
    {
        ClientConfig config;
        config.endpoint_id = endpoint_id;
        config.connect_uri = options.uri;
        config.transport_config.debug = options.debug;
        config.transport_config.time_queue_max_duration = 10000;
        const std::uint64_t idle_multiplier = options.gateway_enabled ? 4 : 2;
        config.transport_config.idle_timeout_ms =
          static_cast<std::uint64_t>(options.timeout.count()) * idle_multiplier;
        return Client::Create(config);
    }

    bool RunPubSubProbe(Client& subscriber,
                        Client& publisher,
                        const Options& options,
                        const std::string& unique_suffix,
                        std::string& error)
    {
        const auto unique_name = options.name.value_or(std::string("probe-") + unique_suffix);
        const auto expected_payload = ToQuicrBytes(options.message);

        FullTrackName track;
        track.name_space = TrackNamespace(laps::relay_health::SplitNamespace(options.name_space));
        track.name = ToQuicrBytes(unique_name);

        const auto result_promise = std::make_shared<std::promise<VerificationResult>>();
        auto result_future = result_promise->get_future();
        auto sub_handler = VerifyingSubscribeTrackHandler::Create(track, expected_payload, result_promise);
        auto pub_handler = PublishTrackHandler::Create(track, TrackMode::kStream, 3, 1000, { 0, 0 });

        subscriber.SubscribeTrack(sub_handler);
        publisher.PublishTrack(pub_handler);

        const bool ready = WaitFor(
          [&sub_handler, &pub_handler]() {
              return sub_handler->GetStatus() == SubscribeTrackHandler::Status::kOk && pub_handler->CanPublish();
          },
          options.timeout);
        if (!ready) {
            std::ostringstream stream;
            stream << "[relay] publisher/subscriber track setup timed out; subscriber status="
                   << static_cast<int>(sub_handler->GetStatus())
                   << ", publisher status=" << static_cast<int>(pub_handler->GetStatus());
            error = stream.str();
            return false;
        }

        ObjectHeaders headers{ .group_id = 0,
                               .object_id = 0,
                               .subgroup_id = 0,
                               .payload_length = expected_payload.size(),
                               .status = ObjectStatus::kAvailable,
                               .priority = 3,
                               .ttl = 1000,
                               .track_mode = TrackMode::kStream,
                               .extensions = std::nullopt,
                               .immutable_extensions = std::nullopt };

        const auto publish_status = pub_handler->PublishObject(headers, expected_payload);
        if (publish_status != PublishTrackHandler::PublishObjectStatus::kOk) {
            error = "[relay] PublishObject failed with status " + std::to_string(static_cast<int>(publish_status));
            return false;
        }

        if (result_future.wait_for(options.timeout) != std::future_status::ready) {
            error = "[relay] subscriber did not receive the health-check object before timeout";
            return false;
        }

        const auto result = result_future.get();
        if (!result.matched) {
            error = "[relay] " + result.error;
            return false;
        }

        return true;
    }

    bool RunSubscribeProbe(Client& subscriber,
                           const FullTrackName& track,
                           const std::string& track_display,
                           const Bytes& expected_payload,
                           std::chrono::milliseconds timeout,
                           std::string& error)
    {
        const auto result_promise = std::make_shared<std::promise<VerificationResult>>();
        auto result_future = result_promise->get_future();
        auto sub_handler = VerifyingSubscribeTrackHandler::Create(track, expected_payload, result_promise);

        subscriber.SubscribeTrack(sub_handler);

        const bool ready = WaitFor(
          [&sub_handler]() { return sub_handler->GetStatus() == SubscribeTrackHandler::Status::kOk; }, timeout);
        if (!ready) {
            std::ostringstream stream;
            stream << "[gateway] subscription setup timed out on " << track_display
                   << "; subscriber status=" << static_cast<int>(sub_handler->GetStatus());
            error = stream.str();
            return false;
        }

        if (result_future.wait_for(timeout) != std::future_status::ready) {
            error = "[gateway] subscription established but no object received on " + track_display + " within " +
                    std::to_string(timeout.count()) + "ms";
            return false;
        }

        const auto result = result_future.get();
        if (!result.matched) {
            error = "[gateway] " + result.error + " (track " + track_display + ")";
            return false;
        }

        return true;
    }

    std::string GatewayTrackDisplay(const std::vector<std::string>& ns_parts, const std::string& name)
    {
        std::string out;
        for (std::size_t i = 0; i < ns_parts.size(); ++i) {
            if (i > 0) {
                out.push_back('/');
            }
            out.append(ns_parts[i]);
        }
        out.push_back('/');
        out.append(name);
        return out;
    }

    bool RunHealthCheck(const Options& options, std::string& error)
    {
        const auto unique_suffix = options.name.value_or(DefaultTrackName());

        auto subscriber = MakeClient("relay-health-subscriber-" + unique_suffix, options);
        auto publisher = MakeClient("relay-health-publisher-" + unique_suffix, options);

        subscriber->Start();
        publisher->Start();

        const bool connected = WaitFor(
          [&subscriber, &publisher]() {
              return subscriber->GetStatus() == Client::Status::kReady &&
                     publisher->GetStatus() == Client::Status::kReady;
          },
          options.timeout);
        if (!connected) {
            error = "[relay] publisher and subscriber did not both connect before timeout";
            subscriber->Stop();
            publisher->Stop();
            return false;
        }

        const bool relay_ok = RunPubSubProbe(*subscriber, *publisher, options, unique_suffix, error);
        if (!relay_ok) {
            subscriber->Stop();
            publisher->Stop();
            return false;
        }

        if (options.gateway_enabled) {
            if (options.gateway_name.empty()) {
                error = "[gateway] LIBQUICR_GATEWAY_HEALTH_NAME (or --gateway-name) is empty";
                subscriber->Stop();
                publisher->Stop();
                return false;
            }

            const auto ns_parts = laps::relay_health::SplitNamespace(options.gateway_namespace);
            FullTrackName gateway_track;
            gateway_track.name_space = TrackNamespace(ns_parts);
            gateway_track.name = ToQuicrBytes(options.gateway_name);

            const auto expected_payload = ToQuicrBytes(options.gateway_message);
            const auto display = GatewayTrackDisplay(ns_parts, options.gateway_name);

            const bool gateway_ok =
              RunSubscribeProbe(*subscriber, gateway_track, display, expected_payload, options.timeout, error);
            if (!gateway_ok) {
                subscriber->Stop();
                publisher->Stop();
                return false;
            }
        }

        subscriber->Stop();
        publisher->Stop();
        return true;
    }
}

int
main(int argc, char* argv[])
{
    try {
        const auto options = laps::relay_health::ParseOptions(argc, argv);
        spdlog::set_level(options.debug ? spdlog::level::debug : spdlog::level::off);

        std::string error;
        if (RunHealthCheck(options, error)) {
            std::cout << "ok\n";
            return EXIT_SUCCESS;
        }

        std::cout << "error\n";
        if (!error.empty()) {
            std::cerr << error << "\n";
        }
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cout << "error\n";
        std::cerr << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
