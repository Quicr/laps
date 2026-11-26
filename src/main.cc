// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "config.h"

#include <condition_variable>
#include <cxxopts.hpp>
#include <filesystem>
#include <set>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "relay.h"
#include "signal_handler.h"

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
    if (cli_opts.count("qlog")) {
        cfg.qlog_path_ = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_LOGGER_INFO(cfg.logger_, "setting debug level");
        spdlog::default_logger()->set_level(spdlog::level::debug);
        cfg.logger_->set_level(spdlog::level::debug);
    }

    if (cli_opts.count("disable_cache") && cli_opts["disable_cache"].as<bool>() == true) {
        SPDLOG_LOGGER_INFO(cfg.logger_, "Disabling object caching");
        cfg.disable_cache = true;
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
            cfg.node_type = moq::fabric::NodeType::kEdge;
        } else if (node_type == "via") {
            cfg.node_type = moq::fabric::NodeType::kVia;
        } else if (node_type == "stub") {
            cfg.node_type = moq::fabric::NodeType::kStub;
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
    cfg.metrics_namespace_ = cli_opts.count("metrics_namespace") ? cli_opts["metrics_namespace"].as<std::string>()
                                                                 : "metrics/" + cfg.relay_id_;
    cfg.admin_namespace_ = cli_opts["admin_namespace"].as<std::string>();

    if (cli_opts.count("cache_key")) {
        cfg.cache_key = cli_opts["cache_key"].as<std::uint64_t>();
    }

    cfg.bind_ip = cli_opts["bind_ip"].as<std::string>();
    cfg.port = cli_opts["port"].as<uint16_t>();
    cfg.cache_duration_ms = cli_opts["cache_duration"].as<size_t>();
}

int
main(int argc, char* argv[])
{
    int result_code = EXIT_SUCCESS;

    laps::Config laps_config;

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
        ("metrics_namespace", "Metrics publish namespace", cxxopts::value<std::string>())
        ("admin_namespace",
            "Admin subscribe-tracks prefix. Slash separates tuple entries. The endpoint id is appended",
            cxxopts::value<std::string>()->default_value("laps/admin"))
        ("l,detached_subs", "Enable support for detached subscribers")
        ("disable_cache", "Disable object caching")
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
    SPDLOG_LOGGER_INFO(laps_config.logger_, "Using metrics namespace: {}", laps_config.metrics_namespace_);
    SPDLOG_LOGGER_INFO(laps_config.logger_, "Using admin namespace prefix: {}", laps_config.admin_namespace_);

    try {
        const Relay relay(laps_config);

        // Wait until told to terminate
        gvars::cv.wait(lock, [&]() { return gvars::terminate; });

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
