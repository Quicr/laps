// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "config.h"

#include <condition_variable>
#include <cxxopts.hpp>
#include <filesystem>
#include <set>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "client_manager.h"
#include "peering/peer_manager.h"
#include "signal_handler.h"
#include "state.h"

#include <quicr/server.h>

using TrackNamespaceHash = uint64_t;
using TrackNameHash = uint64_t;
using FullTrackNameHash = uint64_t;

using namespace laps;

/* -------------------------------------------------------------------------------------------------
 * Main program
 * -------------------------------------------------------------------------------------------------
 */
void
InitConfig(cxxopts::ParseResult& cli_opts, Config& cfg)
{
    cfg.bind_ip = cli_opts["bind_ip"].as<std::string>();
    cfg.port = cli_opts["port"].as<uint16_t>();

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_LOGGER_INFO(cfg.logger_, "setting debug level");
        spdlog::default_logger()->set_level(spdlog::level::debug);
        cfg.logger_->set_level(spdlog::level::debug);
    }

    if (cli_opts.count("detached_subs") && cli_opts["detached_subs"].as<bool>() == true) {
        SPDLOG_LOGGER_INFO(cfg.logger_, "Enabling detached subscriber support");
        cfg.detached_subs = true;
    }

    if (cli_opts.count("allow_self") && cli_opts["allow_self"].as<bool>() == true) {
        SPDLOG_LOGGER_INFO(cfg.logger_, "Enabling subscribe namespace self-subscriptions");
        cfg.allow_self = true;
    }

    if (cli_opts.count("peer")) {
        for (auto& peer : cli_opts["peer"].as<std::vector<std::string>>()) {
            auto port_pos = peer.find(":");
            uint16_t port = kDefaultPeerPort;

            if (port_pos != std::string::npos) {
                port = std::stoi(peer.substr(port_pos + 1, std::string::npos));
            }

            cfg.peering.peers.push_back({ peer.substr(0, port_pos), port });
        }
    }

    if (cli_opts.count("node_type")) {
        const auto& node_type = cli_opts["node_type"].as<std::string>();

        if (node_type == "edge") {
            cfg.node_type = peering::NodeType::kEdge;
        } else if (node_type == "via") {
            cfg.node_type = peering::NodeType::kVia;
        } else if (node_type == "stub") {
            cfg.node_type = peering::NodeType::kStub;
        } else {
            SPDLOG_LOGGER_ERROR(cfg.logger_, "unknown node type: '{}'", node_type);
            exit(-1);
        }

        SPDLOG_LOGGER_INFO(cfg.logger_, "Setting node type to '{}'", node_type);
    }

    cfg.debug = cli_opts["debug"].as<bool>();
    cfg.tls_cert_filename_ = cli_opts["cert"].as<std::string>();
    cfg.tls_key_filename_ = cli_opts["key"].as<std::string>();

    if (!cfg.tls_cert_filename_.empty() && !std::filesystem::exists(cfg.tls_cert_filename_)) {
        SPDLOG_LOGGER_ERROR(cfg.logger_, "TLS certificate file not found: {}", cfg.tls_cert_filename_);
        exit(EXIT_FAILURE);
    }

    if (!cfg.tls_key_filename_.empty() && !std::filesystem::exists(cfg.tls_key_filename_)) {
        SPDLOG_LOGGER_ERROR(cfg.logger_, "TLS key file not found: {}", cfg.tls_key_filename_);
        exit(EXIT_FAILURE);
    }

    cfg.peering.listening_port = cli_opts["peer_port"].as<uint16_t>();
    cfg.object_ttl_ = cli_opts["object_ttl"].as<uint32_t>();
    cfg.sub_dampen_ms_ = cli_opts["sub_dampen_ms"].as<uint32_t>();

    cfg.relay_id_ = cli_opts["endpoint_id"].as<std::string>();

    if (cli_opts.count("cache_key")) {
        cfg.cache_key = cli_opts["cache_key"].as<std::uint64_t>();
    }

    cfg.cache_duration_ms = cli_opts["cache_duration"].as<std::size_t>();
}

int
main(int argc, char* argv[])
{
    int result_code = EXIT_SUCCESS;

    laps::Config laps_config;
    State state;

    SPDLOG_LOGGER_INFO(laps_config.logger_, "Starting LAPS Relay (version {0})", laps_config.Version());

    // clang-format off
    cxxopts::Options options("laps", "Latency Aware Pub/Sub");
    options.set_width(75).set_tab_expansion().allow_unrecognised_options().add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging") // a bool parameter
        ("b,bind_ip", "Bind IP", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("p,port", "Listening port", cxxopts::value<uint16_t>()->default_value(std::to_string(kDefaultClientPort)))
        ("e,endpoint_id", "This relay/server endpoint ID", cxxopts::value<std::string>()->default_value("moq-server"))
        ("c,cert", "Certificate file", cxxopts::value<std::string>()->default_value("./server-cert.pem"))
        ("k,key", "Certificate key file", cxxopts::value<std::string>()->default_value("./server-key.pem"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,sub_dampen_ms", "Subscription update dampen interval in milliseconds",
            cxxopts::value<uint32_t>()->default_value(std::to_string(kDefaultCacheTimeQueueMaxDuration)))
        ("t,object_ttl", "Object TTL in milliseconds",
            cxxopts::value<uint32_t>()->default_value(std::to_string(kDefaultObjectTtl)))
        ("cache_duration",
            "Duration of cache objects in milliseconds",
            cxxopts::value<size_t>()->default_value("60000"))
        ("cache_key", "Value of isCached extension key", cxxopts::value<std::uint64_t>())
        ("l,detached_subs", "Enable support for detached subscribers")
        ("allow_self", "Allow subscribe namespace self-subscriptions");


    options.add_options("Peering")
        ("peer_port", "Listening port for peering connections",
            cxxopts::value<uint16_t>()->default_value(std::to_string(kDefaultPeerPort)))
        ("peer", "Peer array host[:port],...", cxxopts::value<std::vector<std::string>>())
        ("node_type", "Peer type as 'edge', 'via', 'stub'. Default is edge",
            cxxopts::value<std::string>());

    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Peering" }) << std::endl;
        return EXIT_SUCCESS;
    }

    // Install a signal handlers to catch operating system signals
    installSignalHandlers();

    // Lock the mutex so that main can then wait on it
    std::unique_lock<std::mutex> lock(gvars::main_mutex);

    InitConfig(result, laps_config);

    std::shared_ptr<peering::InfoBase> forwarding_info = std::make_shared<peering::InfoBase>();
    peering::PeerManager peer_manager(laps_config, state, forwarding_info);

    try {
        auto server = laps::MakeClientManager(state, laps_config, peer_manager);

        if (!server->Start()) {
            SPDLOG_ERROR("Server failed to start");
            exit(-2);
        }

        // Wait until told to terminate
        gvars::cv.wait(lock, [&]() { return gvars::terminate; });

        // Unlock the mutex
        lock.unlock();
    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unexpected exception" << std::endl;
        result_code = EXIT_FAILURE;
    }

    return result_code;
}
