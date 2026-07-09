// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "quicr/client.h"
#include "quicr/publish_track_handler.h"
#include "quicr/subscribe_track_handler.h"

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

namespace {

    struct Options
    {
        std::string uri{ "moq://laps-relay:12345/relay" };
        std::chrono::milliseconds timeout{ 5000 };
        std::string name_space{ "libquicr/health" };
        std::optional<std::string> name;
        std::string message{ "libquicr relay health check" };
        bool debug{ false };
    };

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

    std::string GetEnvOrDefault(const char* key, std::string fallback)
    {
        const auto value = std::getenv(key);
        if (value == nullptr || std::string_view(value).empty()) {
            return fallback;
        }
        return value;
    }

    std::chrono::milliseconds ParseTimeout(std::string_view value)
    {
        try {
            return std::chrono::milliseconds(std::stoul(std::string(value)));
        } catch (const std::exception&) {
            throw std::runtime_error("timeout must be an integer number of milliseconds");
        }
    }

    std::vector<std::string> SplitNamespace(std::string_view value)
    {
        std::vector<std::string> parts;
        std::string current;
        for (const char ch : value) {
            if (ch == '/' || ch == ',') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }

        if (!current.empty()) {
            parts.push_back(current);
        }

        if (parts.empty()) {
            parts.emplace_back("health");
        }

        return parts;
    }

    Bytes ToBytes(std::string_view value)
    {
        Bytes bytes;
        bytes.reserve(value.size());
        for (const auto ch : value) {
            bytes.push_back(static_cast<std::uint8_t>(ch));
        }
        return bytes;
    }

    std::string MakeDefaultTrackName()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "probe-" + std::to_string(now);
    }

    void ApplyEnvironment(Options& options)
    {
        options.uri = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_URI", options.uri);
        options.name_space = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_NAMESPACE", options.name_space);
        options.message = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_MESSAGE", options.message);

        if (const auto name = std::getenv("LIBQUICR_RELAY_HEALTH_NAME"); name != nullptr && *name != '\0') {
            options.name = name;
        }

        if (const auto timeout = std::getenv("LIBQUICR_RELAY_HEALTH_TIMEOUT_MS");
            timeout != nullptr && *timeout != '\0') {
            options.timeout = ParseTimeout(timeout);
        }

        if (const auto debug = std::getenv("LIBQUICR_RELAY_HEALTH_DEBUG"); debug != nullptr) {
            options.debug = std::string_view(debug) == "1" || std::string_view(debug) == "true";
        }
    }

    void PrintUsage(const char* program)
    {
        std::cerr << "Usage: " << program
                  << " [--uri URI] [--timeout-ms MS] [--namespace NS] [--name NAME] [--message TEXT] [--debug]\n"
                  << "\n"
                  << "Environment overrides:\n"
                  << "  LIBQUICR_RELAY_HEALTH_URI\n"
                  << "  LIBQUICR_RELAY_HEALTH_TIMEOUT_MS\n"
                  << "  LIBQUICR_RELAY_HEALTH_NAMESPACE\n"
                  << "  LIBQUICR_RELAY_HEALTH_NAME\n"
                  << "  LIBQUICR_RELAY_HEALTH_MESSAGE\n"
                  << "  LIBQUICR_RELAY_HEALTH_DEBUG\n";
    }

    Options ParseOptions(int argc, char* argv[])
    {
        Options options;
        ApplyEnvironment(options);

        auto require_value = [&](int& index, const std::string_view option) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            ++index;
            return argv[index];
        };

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                PrintUsage(argv[0]);
                std::exit(EXIT_SUCCESS);
            }
            if (arg == "--uri") {
                options.uri = require_value(i, arg);
            } else if (arg == "--timeout-ms") {
                options.timeout = ParseTimeout(require_value(i, arg));
            } else if (arg == "--namespace") {
                options.name_space = require_value(i, arg);
            } else if (arg == "--name") {
                options.name = require_value(i, arg);
            } else if (arg == "--message") {
                options.message = require_value(i, arg);
            } else if (arg == "--debug") {
                options.debug = true;
            } else {
                throw std::runtime_error("unknown option: " + std::string(arg));
            }
        }

        return options;
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
        config.transport_config.idle_timeout_ms = static_cast<std::uint64_t>(options.timeout.count() * 2);
        return Client::Create(config);
    }

    bool RunHealthCheck(const Options& options, std::string& error)
    {
        const auto unique_name = options.name.value_or(MakeDefaultTrackName());
        const auto unique_suffix = unique_name;
        const auto expected_payload = ToBytes(options.message);

        FullTrackName track;
        track.name_space = TrackNamespace(SplitNamespace(options.name_space));
        track.name = ToBytes(unique_name);

        auto subscriber = MakeClient("relay-health-subscriber-" + unique_suffix, options);
        auto publisher = MakeClient("relay-health-publisher-" + unique_suffix, options);

        subscriber->Connect();
        publisher->Connect();

        const bool connected = WaitFor(
          [&subscriber, &publisher]() {
              return subscriber->GetStatus() == Transport::Status::kReady &&
                     publisher->GetStatus() == Transport::Status::kReady;
          },
          options.timeout);
        if (!connected) {
            error = "publisher and subscriber did not both connect before timeout";
            subscriber->Disconnect();
            publisher->Disconnect();
            return false;
        }

        const auto result_promise = std::make_shared<std::promise<VerificationResult>>();
        auto result_future = result_promise->get_future();
        auto sub_handler = VerifyingSubscribeTrackHandler::Create(track, expected_payload, result_promise);
        auto pub_handler = PublishTrackHandler::Create(track, TrackMode::kStream, 3, 1000, { 0, 0 });

        subscriber->SubscribeTrack(sub_handler);
        publisher->PublishTrack(pub_handler);

        const bool ready = WaitFor(
          [&sub_handler, &pub_handler]() {
              return sub_handler->GetStatus() == SubscribeTrackHandler::Status::kOk && pub_handler->CanPublish();
          },
          options.timeout);
        if (!ready) {
            std::ostringstream stream;
            stream << "publisher/subscriber track setup timed out; subscriber status="
                   << static_cast<int>(sub_handler->GetStatus())
                   << ", publisher status=" << static_cast<int>(pub_handler->GetStatus());
            error = stream.str();
            subscriber->Disconnect();
            publisher->Disconnect();
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
            error = "PublishObject failed with status " + std::to_string(static_cast<int>(publish_status));
            subscriber->Disconnect();
            publisher->Disconnect();
            return false;
        }

        if (result_future.wait_for(options.timeout) != std::future_status::ready) {
            error = "subscriber did not receive the health-check object before timeout";
            subscriber->Disconnect();
            publisher->Disconnect();
            return false;
        }

        const auto result = result_future.get();
        subscriber->Disconnect();
        publisher->Disconnect();

        if (!result.matched) {
            error = result.error;
            return false;
        }

        return true;
    }
}

int
main(int argc, char* argv[])
{
    try {
        const auto options = ParseOptions(argc, argv);
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
