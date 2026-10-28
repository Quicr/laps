// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "admin_codec.h"
#include "spdlog_logger.h"

#include <quicr/handlers/publish_namespace_handler.h>
#include <quicr/handlers/publish_track_handler.h>
#include <quicr/handlers/subscribe_track_handler.h>
#include <quicr/messages/object.h>
#include <quicr/session.h>
#include <quicr/session_callbacks.h>
#include <quicr/session_manager.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
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
        /// Unset until --target-endpoint is passed. Otherwise taken from the relay SETUP endpoint id.
        std::optional<std::string> target_endpoint;
        std::string endpoint_id;
        struct Call
        {
            std::string method;
            std::optional<std::string> params_json;
        };
        std::vector<Call> calls;
        std::chrono::milliseconds timeout{ 5000 };
        /// Unset writes the JSON document to stdout.
        std::optional<std::string> out_path;
        bool debug{ false };
    };

    class ReplyHandler final : public quicr::SubscribeTrackHandler
    {
      public:
        static std::shared_ptr<ReplyHandler> Create(const quicr::FullTrackName& full_track_name)
        {
            return std::shared_ptr<ReplyHandler>(new ReplyHandler(full_track_name));
        }

        std::optional<std::string> Wait(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait_for(lock, timeout, [this]() { return !replies_.empty(); })) {
                return std::nullopt;
            }
            auto payload = std::move(replies_.front());
            replies_.pop_front();
            return payload;
        }

        void ObjectReceived(const quicr::ObjectHeaders&,
                            quicr::BytesSpan data,
                            std::optional<quicr::messages::StreamHeaderProperties>) override
        {
            if (data.empty()) {
                return;
            }

            std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
            std::lock_guard lock(mutex_);
            replies_.push_back(std::move(payload));
            ready_.notify_one();
        }

      private:
        explicit ReplyHandler(const quicr::FullTrackName& full_track_name)
          : quicr::SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending)
        {
        }

        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<std::string> replies_;
    };

    void PrintUsage(std::ostream& out, const char* program)
    {
        out << "Usage: " << program << " [options] <method> [params] [<method> [params] ...]\n"
            << "\n"
            << "Talk to a LAPS relay admin RPC. The client publishes a request on\n"
            << "  <admin-namespace>/<target-endpoint>/<endpoint-id>  name rpc\n"
            << "and subscribes to the reply on\n"
            << "  <admin-namespace>/<endpoint-id>/<target-endpoint>  name rpc\n"
            << "\n"
            << "Several methods may be given. Each runs in order on one session.\n"
            << "A method that takes parameters reads the next argument. Separate\n"
            << "multiple values in that argument with commas. Each value is one call.\n"
            << "\n"
            << "Methods:\n"
            << "  state                           JSON sizes of the relay's internal maps\n"
            << "  peers                           JSON list of accepted and dialed peers\n"
            << "  add-peer <host>:<port>          Dial a peer, or remember an accepted one\n"
            << "  remove-peer <host>:<port>       Disconnect a peer and drop its state\n"
            << "  restart-peer <host>:<port>      Drop a peer's state. Dialed peers reconnect\n"
            << "\n"
            << "Options:\n"
            << "  --uri URI                  Relay URI (default moq://127.0.0.1:33435)\n"
            << "  --target-endpoint ID       Target relay endpoint (default: the id from relay SETUP)\n"
            << "  --endpoint-id ID           This admin client endpoint id (default admin-<pid>)\n"
            << "  --admin-namespace PREFIX   Slash-delimited base prefix (default laps/admin)\n"
            << "  --timeout-ms MS            How long to wait for each response (default 5000)\n"
            << "  --out FILE                 Write the JSON document to FILE instead of stdout\n"
            << "  --debug\n"
            << "  -h, --help\n";
    }

    bool MethodTakesParams(std::string_view method)
    {
        return method == laps::kAdminAddPeerMethod || method == laps::kAdminRemovePeerMethod ||
               method == laps::kAdminRestartPeerMethod;
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

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                PrintUsage(std::cout, argv[0]);
                std::exit(EXIT_SUCCESS);
            }
            if (arg == "--uri") {
                options.uri = RequireValue(argc, argv, i, arg);
            } else if (arg == "--target-endpoint") {
                options.target_endpoint = RequireValue(argc, argv, i, arg);
            } else if (arg == "--endpoint-id") {
                options.endpoint_id = RequireValue(argc, argv, i, arg);
            } else if (arg == "--admin-namespace") {
                options.admin_namespace = RequireValue(argc, argv, i, arg);
            } else if (arg == "--timeout-ms") {
                const auto text = RequireValue(argc, argv, i, arg);
                std::size_t consumed = 0;
                const auto parsed = std::stoul(text, &consumed);
                if (consumed != text.size()) {
                    throw std::runtime_error("timeout must be an integer number of milliseconds");
                }
                options.timeout = std::chrono::milliseconds(parsed);
            } else if (arg == "--out") {
                options.out_path = RequireValue(argc, argv, i, arg);
                if (options.out_path->empty()) {
                    throw std::runtime_error("--out requires a file path");
                }
            } else if (arg == "--debug") {
                options.debug = true;
            } else if (!arg.empty() && arg.front() == '-') {
                throw std::runtime_error("unknown option: " + std::string(arg));
            } else {
                const auto method = std::string(arg);
                if (!MethodTakesParams(method)) {
                    options.calls.push_back(Options::Call{ method, std::nullopt });
                    continue;
                }
                if (i + 1 >= argc) {
                    throw std::runtime_error(method + " requires host:port");
                }
                ++i;
                const auto parts = laps::SplitAdminParams(argv[i]);
                if (parts.empty()) {
                    throw std::runtime_error(method + " requires host:port");
                }
                for (const auto& part : parts) {
                    if (part.empty()) {
                        throw std::runtime_error(method + " has an empty parameter");
                    }
                    std::string error;
                    const auto endpoint = laps::ParseAdminPeerEndpoint(part, error);
                    if (!endpoint.has_value()) {
                        throw std::runtime_error(method + " " + error + ": " + part);
                    }
                    options.calls.push_back(Options::Call{ method, laps::EncodeAdminPeerParams(*endpoint) });
                }
            }
        }

        if (options.calls.empty()) {
            throw std::runtime_error("a method is required");
        }
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

        auto logger = spdlog::stderr_color_mt("lapsAdmin");
        logger->set_level(options.debug ? spdlog::level::debug : spdlog::level::info);
        auto quicr_logger = std::make_shared<laps::SpdlogLogger>(logger);

        quicr::ClientConfig config;
        config.endpoint_id = options.endpoint_id;
        config.connect_uri = options.uri;
        config.transport_config.debug = options.debug;
        config.transport_config.time_queue_max_duration = 10000;
        config.transport_config.idle_timeout_ms = static_cast<std::uint64_t>(options.timeout.count()) * 2;

        std::vector<laps::AdminResultCall> replies;
        std::string relay_endpoint;
        std::vector<std::string> errors;
        bool failed = false;
        std::optional<std::uint64_t> reported_version;
        bool reported_missing_version = false;
        {
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

            const auto connected_endpoint = setup_endpoint.get();
            if (connected_endpoint.empty()) {
                session->Disconnect();
                throw std::runtime_error("relay SETUP did not include an endpoint id");
            }
            if (options.target_endpoint.has_value()) {
                if (options.target_endpoint->empty()) {
                    session->Disconnect();
                    throw std::runtime_error("--target-endpoint requires an endpoint id");
                }
                relay_endpoint = *options.target_endpoint;
                if (relay_endpoint != connected_endpoint) {
                    SPDLOG_LOGGER_INFO(logger,
                                       "Connected to relay {}; admin namespace uses {}",
                                       connected_endpoint,
                                       relay_endpoint);
                }
            } else {
                relay_endpoint = connected_endpoint;
            }

            std::string layout_error;
            const auto layout =
              laps::MakeAdminTrackLayout(options.admin_namespace, relay_endpoint, options.endpoint_id, layout_error);
            if (!layout.has_value()) {
                session->Disconnect();
                throw std::runtime_error(layout_error);
            }

            const auto request_base = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();

            SPDLOG_LOGGER_INFO(logger,
                               "Admin request {} -> {} name {}",
                               laps::JoinTrackNamespace(layout->request),
                               laps::JoinTrackNamespace(layout->reply),
                               laps::kAdminRpcTrackName);

            const auto reply_track = laps::AdminFullTrackName(layout->reply);
            const auto request_track = laps::AdminFullTrackName(layout->request);
            auto reply = ReplyHandler::Create(reply_track);
            auto request_namespace = quicr::PublishNamespaceHandler::Create(request_track.name_space);
            auto publish =
              quicr::PublishTrackHandler::Create(request_track, quicr::TrackMode::kStream, 3, 3, 1000, { 0, 0 });

            // Subscribe to the reply first, the same way a metrics reader subscribes to a known track.
            // Publish the request namespace and track so peering can announce them to the target relay.
            session->SubscribeTrack(reply);
            session->PublishNamespace(request_namespace);
            session->PublishTrack(publish);

            const bool ready = WaitFor([&publish]() { return publish->CanPublish(); }, options.timeout);
            if (!ready || !publish->CanPublish()) {
                const auto status = publish->GetStatus();
                session->Disconnect();
                const auto request_ns = laps::JoinTrackNamespace(layout->request);
                throw std::runtime_error("admin request " + request_ns +
                                         " (rpc) was not accepted before timeout (publish status " +
                                         std::string(PublishStatusName(status)) +
                                         "). The target relay subscribes to this namespace through peering.");
            }

            for (std::size_t index = 0; index < options.calls.size(); ++index) {
                const auto& call = options.calls[index];
                const auto request_id = std::to_string(request_base) + "-" + std::to_string(index);
                const auto request_json = laps::EncodeAdminRequest(
                  request_id,
                  call.method,
                  call.params_json.has_value() ? std::optional<std::string_view>(*call.params_json) : std::nullopt);
                SPDLOG_LOGGER_DEBUG(logger, "Admin request body {}", request_json);

                const std::vector<std::uint8_t> payload(request_json.begin(), request_json.end());
                const quicr::ObjectHeaders headers{ .group_id = static_cast<std::uint64_t>(index),
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

                const auto response = reply->Wait(options.timeout);
                if (!response.has_value()) {
                    failed = true;
                    errors.push_back("timed out waiting for the admin response to " + call.method);
                    replies.push_back(
                      laps::AdminResultCall{ call.method,
                                             R"({"error":{"message":"timed out waiting for the admin response"}})" });
                    break;
                }

                replies.push_back(laps::AdminResultCall{ call.method, *response });
                const auto parsed = laps::ParseAdminMessage(*response);
                if (!parsed.has_api_version) {
                    reported_missing_version = true;
                } else {
                    reported_version = parsed.api_version;
                }
                if (!parsed.valid || !parsed.has_result || parsed.has_error) {
                    failed = true;
                    if (!parsed.error_message.empty()) {
                        errors.push_back(parsed.error_message);
                    }
                }
            }

            session->Disconnect();
            publish.reset();
            request_namespace.reset();
            reply.reset();
            session.reset();
        }

        logger->flush();

        if (reported_missing_version && !reported_version.has_value()) {
            SPDLOG_LOGGER_WARN(logger,
                               "Relay did not report an admin API version; this client speaks version {}",
                               laps::kAdminApiVersion);
        } else if (reported_version.has_value() && *reported_version != laps::kAdminApiVersion) {
            SPDLOG_LOGGER_WARN(logger,
                               "Relay admin API version {}; this client speaks version {}",
                               *reported_version,
                               laps::kAdminApiVersion);
        } else if (reported_version.has_value()) {
            SPDLOG_LOGGER_INFO(logger, "Relay admin API version {}", *reported_version);
        }
        logger->flush();

        for (const auto& error : errors) {
            std::cerr << error << '\n';
        }

        if (!replies.empty()) {
            const auto document = laps::EncodeAdminResultsDocument(replies, relay_endpoint);
            const auto pretty = laps::PrettyPrintJson(document);
            auto printed = pretty.has_value() ? *pretty : document;
            if (printed.empty() || printed.back() != '\n') {
                printed.push_back('\n');
            }
            if (options.out_path.has_value()) {
                std::ofstream file(*options.out_path, std::ios::out | std::ios::trunc);
                if (!file) {
                    throw std::runtime_error("failed to open " + *options.out_path);
                }
                file << printed;
                if (!file) {
                    throw std::runtime_error("failed to write " + *options.out_path);
                }
            } else {
                std::cout << printed;
                std::cout.flush();
            }
        }

        if (failed || replies.empty()) {
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
