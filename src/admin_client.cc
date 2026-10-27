// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "admin_codec.h"
#include "spdlog_logger.h"

#include <quicr/handlers/publish_track_handler.h>
#include <quicr/handlers/subscribe_track_handler.h>
#include <quicr/messages/object.h>
#include <quicr/session.h>
#include <quicr/session_callbacks.h>
#include <quicr/session_manager.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
    struct Options
    {
        std::string uri{ "moq://127.0.0.1:33435" };
        std::string admin_namespace{ std::string(laps::kDefaultAdminPrefix) };
        /// Unset until --relay-endpoint is passed. Otherwise taken from the relay SETUP endpoint id.
        std::optional<std::string> relay_endpoint;
        std::string endpoint_id;
        std::string method{ laps::kAdminStateSizesMethod };
        std::optional<std::string> params;
        std::chrono::milliseconds timeout{ 5000 };
        bool debug{ false };
    };

    class ReplyHandler final : public quicr::SubscribeTrackHandler
    {
      public:
        static std::shared_ptr<ReplyHandler> Create(const quicr::FullTrackName& full_track_name)
        {
            return std::shared_ptr<ReplyHandler>(new ReplyHandler(full_track_name));
        }

        std::future<std::string> Response() { return promise_.get_future(); }

        void ObjectReceived(const quicr::ObjectHeaders&,
                            quicr::BytesSpan data,
                            std::optional<quicr::messages::StreamHeaderProperties>) override
        {
            if (data.empty()) {
                return;
            }

            std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
            std::lock_guard lock(mutex_);
            if (completed_) {
                return;
            }
            completed_ = true;
            promise_.set_value(std::move(payload));
        }

      private:
        explicit ReplyHandler(const quicr::FullTrackName& full_track_name)
          : quicr::SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending)
        {
        }

        std::mutex mutex_;
        bool completed_{ false };
        std::promise<std::string> promise_;
    };

    void PrintUsage(std::ostream& out, const char* program)
    {
        out << "Usage: " << program << " [options] <method>\n"
            << "\n"
            << "Talk to a LAPS relay admin RPC. The client publishes a request on\n"
            << "  <admin-namespace>/<relay-endpoint>/<endpoint-id>  name rpc\n"
            << "and subscribes to the reply on\n"
            << "  <admin-namespace>/<endpoint-id>/<relay-endpoint>  name rpc\n"
            << "\n"
            << "Methods:\n"
            << "  state, state.sizes    JSON sizes of the relay's internal maps\n"
            << "\n"
            << "Options:\n"
            << "  --uri URI                  Relay URI (default moq://127.0.0.1:33435)\n"
            << "  --relay-endpoint ID        Relay --endpoint_id (default: the id from relay SETUP)\n"
            << "  --endpoint-id ID           This admin client endpoint id (default admin-<pid>)\n"
            << "  --admin-namespace PREFIX   Slash-delimited base prefix (default laps/admin)\n"
            << "  --params JSON              Optional request params, one JSON value\n"
            << "  --timeout-ms MS            How long to wait for the response (default 5000)\n"
            << "  --debug\n"
            << "  -h, --help\n";
    }

    std::string RequireValue(int argc, char* argv[], int& index, std::string_view option)
    {
        if (index + 1 >= argc) {
            throw std::runtime_error(std::string(option) + " requires a value");
        }
        ++index;
        return argv[index];
    }

    Options ParseArgs(int argc, char* argv[])
    {
        Options options;
        options.endpoint_id = "admin-" + std::to_string(static_cast<long long>(::getpid()));
        std::optional<std::string> method;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                PrintUsage(std::cout, argv[0]);
                std::exit(EXIT_SUCCESS);
            }
            if (arg == "--uri") {
                options.uri = RequireValue(argc, argv, i, arg);
            } else if (arg == "--relay-endpoint") {
                options.relay_endpoint = RequireValue(argc, argv, i, arg);
            } else if (arg == "--endpoint-id") {
                options.endpoint_id = RequireValue(argc, argv, i, arg);
            } else if (arg == "--admin-namespace") {
                options.admin_namespace = RequireValue(argc, argv, i, arg);
            } else if (arg == "--params") {
                options.params = RequireValue(argc, argv, i, arg);
            } else if (arg == "--timeout-ms") {
                const auto text = RequireValue(argc, argv, i, arg);
                std::size_t consumed = 0;
                const auto parsed = std::stoul(text, &consumed);
                if (consumed != text.size()) {
                    throw std::runtime_error("timeout must be an integer number of milliseconds");
                }
                options.timeout = std::chrono::milliseconds(parsed);
            } else if (arg == "--debug") {
                options.debug = true;
            } else if (!arg.empty() && arg.front() == '-') {
                throw std::runtime_error("unknown option: " + std::string(arg));
            } else if (method.has_value()) {
                throw std::runtime_error("unexpected argument: " + std::string(arg));
            } else {
                method = std::string(arg);
            }
        }

        if (!method.has_value()) {
            throw std::runtime_error("a method is required");
        }
        options.method = *method == "state" ? std::string(laps::kAdminStateSizesMethod) : *method;
        return options;
    }

    /// Remembers the endpoint id from the relay SETUP message.
    class SetupCallback final : public quicr::Session::ClientCallbacks
    {
      public:
        quicr::Expected<void, quicr::Error<quicr::ErrorCode>> SetupReceived(
          const std::shared_ptr<quicr::Session>&,
          const quicr::SetupAttributes& setup) override
        {
            std::lock_guard lock(mutex_);
            if (!completed_) {
                completed_ = true;
                endpoint_.set_value(setup.endpoint_id);
            }
            return {};
        }

        std::future<std::string> Endpoint() { return endpoint_.get_future(); }

      private:
        std::mutex mutex_;
        bool completed_{ false };
        std::promise<std::string> endpoint_;
    };

    const char* PublishStatusName(quicr::PublishTrackHandler::Status status)
    {
        using Status = quicr::PublishTrackHandler::Status;
        switch (status) {
            case Status::kOk:
                return "ok";
            case Status::kNotConnected:
                return "not connected";
            case Status::kNotAnnounced:
                return "not announced";
            case Status::kPendingAnnounceResponse:
                return "pending announce";
            case Status::kAnnounceNotAuthorized:
                return "announce not authorized";
            case Status::kNoSubscribers:
                return "no subscribers";
            case Status::kUnsubscribed:
                return "unsubscribed";
            case Status::kDoneByFin:
                return "done";
            case Status::kSendingUnannounce:
                return "sending unannounce";
            case Status::kSubscriptionUpdated:
                return "subscription updated";
            case Status::kNewGroupRequested:
                return "new group requested";
            case Status::kPendingPublishOk:
                return "pending publish ok";
            case Status::kPaused:
                return "paused";
        }
        return "unknown";
    }

    template<typename Predicate>
    bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    }
} // namespace

int
main(int argc, char* argv[])
{
    try {
        const auto options = ParseArgs(argc, argv);
        if (options.params.has_value()) {
            std::string params_error;
            if (!laps::IsJsonValue(*options.params, params_error)) {
                throw std::runtime_error("params are not a JSON value: " + params_error);
            }
        }

        auto logger = spdlog::stderr_color_mt("lapsAdmin");
        logger->set_level(options.debug ? spdlog::level::debug : spdlog::level::info);
        auto quicr_logger = std::make_shared<laps::SpdlogLogger>(logger);

        quicr::ClientConfig config;
        config.endpoint_id = options.endpoint_id;
        config.connect_uri = options.uri;
        config.transport_config.debug = options.debug;
        config.transport_config.time_queue_max_duration = 10000;
        config.transport_config.idle_timeout_ms = static_cast<std::uint64_t>(options.timeout.count()) * 2;

        auto setup_callback = std::make_shared<SetupCallback>();
        auto setup_endpoint = setup_callback->Endpoint();
        quicr::SessionManager session_manager(quicr_logger);
        auto session = session_manager.AddTransport(config, setup_callback).lock();
        if (session == nullptr) {
            throw std::runtime_error("failed to create the admin session");
        }

        const bool connected =
          WaitFor([&session]() { return session->GetStatus() == quicr::Session::Status::kReady; }, options.timeout);
        if (!connected) {
            session->Disconnect();
            throw std::runtime_error("timed out connecting to " + options.uri);
        }
        if (setup_endpoint.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            session->Disconnect();
            throw std::runtime_error("connected to " + options.uri + " without a relay SETUP endpoint id");
        }

        const auto relay_endpoint = setup_endpoint.get();
        if (relay_endpoint.empty()) {
            session->Disconnect();
            throw std::runtime_error("relay SETUP did not include an endpoint id");
        }
        if (options.relay_endpoint.has_value() && *options.relay_endpoint != relay_endpoint) {
            session->Disconnect();
            throw std::runtime_error("relay at " + options.uri + " identified itself as '" + relay_endpoint +
                                     "', but --relay-endpoint is '" + *options.relay_endpoint +
                                     "'. This relay subscribes to " + options.admin_namespace + "/" + relay_endpoint +
                                     ". Retry with --relay-endpoint " + relay_endpoint);
        }

        std::string layout_error;
        const auto layout =
          laps::MakeAdminTrackLayout(options.admin_namespace, relay_endpoint, options.endpoint_id, layout_error);
        if (!layout.has_value()) {
            session->Disconnect();
            throw std::runtime_error(layout_error);
        }

        const auto request_id = std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
        const auto request_json = laps::EncodeAdminRequest(
          request_id,
          options.method,
          options.params.has_value() ? std::optional<std::string_view>(*options.params) : std::nullopt);

        SPDLOG_LOGGER_INFO(logger,
                           "Admin request {} -> {} name {}",
                           laps::JoinTrackNamespace(layout->request),
                           laps::JoinTrackNamespace(layout->reply),
                           laps::kAdminRpcTrackName);
        SPDLOG_LOGGER_DEBUG(logger, "Admin request body {}", request_json);

        const auto reply_track = laps::AdminFullTrackName(layout->reply);
        const auto request_track = laps::AdminFullTrackName(layout->request);
        auto reply = ReplyHandler::Create(reply_track);
        auto response = reply->Response();
        auto publish =
          quicr::PublishTrackHandler::Create(request_track, quicr::TrackMode::kStream, 3, 3, 1000, { 0, 0 });

        session->SubscribeTrack(reply);
        session->PublishTrack(publish);

        const bool ready = WaitFor(
          [&publish]() {
              const auto status = publish->GetStatus();
              return publish->CanPublish() || status == quicr::PublishTrackHandler::Status::kPaused ||
                     status == quicr::PublishTrackHandler::Status::kNoSubscribers;
          },
          options.timeout);
        if (!ready || !publish->CanPublish()) {
            const auto status = publish->GetStatus();
            session->Disconnect();
            const auto request_ns = laps::JoinTrackNamespace(layout->request);
            if (status == quicr::PublishTrackHandler::Status::kPaused ||
                status == quicr::PublishTrackHandler::Status::kNoSubscribers) {
                throw std::runtime_error("relay paused " + request_ns + " (rpc). It subscribes to " +
                                         options.admin_namespace + "/" + relay_endpoint +
                                         ", so --relay-endpoint must be that endpoint id (publish status " +
                                         PublishStatusName(status) + ")");
            }
            throw std::runtime_error("admin request track was not ready before timeout (publish status " +
                                     std::string(PublishStatusName(status)) + ")");
        }

        const std::vector<std::uint8_t> payload(request_json.begin(), request_json.end());
        const quicr::ObjectHeaders headers{ .group_id = 0,
                                            .object_id = 0,
                                            .subgroup_id = 0,
                                            .payload_length = payload.size(),
                                            .status = quicr::ObjectStatus::kAvailable,
                                            .priority = 3,
                                            .ttl = 1000,
                                            .track_mode = quicr::TrackMode::kStream,
                                            .extensions = std::nullopt,
                                            .immutable_extensions = std::nullopt };
        const auto publish_status = publish->PublishObject(headers, payload);
        if (publish_status != quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
            session->Disconnect();
            throw std::runtime_error("failed to publish the admin request, status " +
                                     std::to_string(static_cast<int>(publish_status)));
        }

        if (response.wait_for(options.timeout) != std::future_status::ready) {
            session->Disconnect();
            throw std::runtime_error("timed out waiting for the admin response");
        }

        const auto body = response.get();
        session->Disconnect();
        const auto pretty = laps::PrettyPrintJson(body);
        const auto& printed = pretty.has_value() ? *pretty : body;
        std::cout << printed;
        if (printed.empty() || printed.back() != '\n') {
            std::cout << '\n';
        }

        const auto parsed = laps::ParseAdminMessage(body);
        if (!parsed.valid || !parsed.has_result || parsed.has_error) {
            if (!parsed.error_message.empty()) {
                std::cerr << parsed.error_message << '\n';
            }
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        if (std::string_view(ex.what()) == "a method is required") {
            PrintUsage(std::cerr, argv[0]);
        }
        return EXIT_FAILURE;
    }
}
