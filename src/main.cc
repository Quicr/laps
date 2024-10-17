// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "config.h"

#include <condition_variable>
#include <cxxopts.hpp>
#include <set>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unordered_map>

#include "server.h"
#include "signal_handler.h"
#include "subscribe_handler.h"
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
quicr::ServerConfig
InitConfig(cxxopts::ParseResult& cli_opts)
{
    quicr::ServerConfig config;

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_INFO("setting debug level");
        spdlog::default_logger()->set_level(spdlog::level::debug);
    }

    config.endpoint_id = cli_opts["endpoint_id"].as<std::string>();

    config.server_bind_ip = cli_opts["bind_ip"].as<std::string>();
    config.server_port = cli_opts["port"].as<uint16_t>();

    config.transport_config.debug = cli_opts["debug"].as<bool>();
    config.transport_config.tls_cert_filename = cli_opts["cert"].as<std::string>();
    config.transport_config.tls_key_filename = cli_opts["key"].as<std::string>();
    config.transport_config.use_reset_wait_strategy = false;
    config.transport_config.time_queue_max_duration = 5000;
    config.transport_config.quic_qlog_path = qlog_path;

    return config;
}

int
main(int argc, char* argv[])
{
    int result_code = EXIT_SUCCESS;

    laps::Config cfg;
    State state;

    SPDLOG_LOGGER_INFO(cfg.logger_, "Starting LAPS Relay (version {0})", cfg.Version());

    cxxopts::Options options("qclient", "MOQ Example Client");
    options.set_width(75).set_tab_expansion().allow_unrecognised_options().add_options()("h,help", "Print help")(
      "d,debug", "Enable debugging") // a bool parameter
      ("b,bind_ip", "Bind IP", cxxopts::value<std::string>()->default_value("127.0.0.1"))(
        "p,port", "Listening port", cxxopts::value<uint16_t>()->default_value("1234"))(
        "e,endpoint_id", "This relay/server endpoint ID", cxxopts::value<std::string>()->default_value("moq-server"))(
        "c,cert", "Certificate file", cxxopts::value<std::string>()->default_value("./server-cert.pem"))(
        "k,key", "Certificate key file", cxxopts::value<std::string>()->default_value("./server-key.pem"))(
        "q,qlog", "Enable qlog using path", cxxopts::value<std::string>()); // end of options

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "" }) << std::endl;
        return EXIT_SUCCESS;
    }

    // Install a signal handlers to catch operating system signals
    installSignalHandlers();

    // Lock the mutex so that main can then wait on it
    std::unique_lock<std::mutex> lock(moq_example::main_mutex);

    quicr::ServerConfig config = InitConfig(result);

    try {
        auto server = std::make_shared<LapsServer>(state, config);
        if (server->Start() != quicr::Transport::Status::kReady) {
            SPDLOG_ERROR("Server failed to start");
            exit(-2);
        }

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

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
