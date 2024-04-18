
#include "config.h"
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>

#include <unistd.h>

namespace laps {

    Config::Config()
      : logger(std::make_shared<cantina::Logger>("lapsRelay"))
    {
        init_defaults();
        cfg_from_env();
        init_logger();
    }

    void Config::init_defaults()
    {
        client_config.bind_addr = "127.0.0.1";
        client_config.listen_port = 33434;
        client_config.protocol = quicr::RelayInfo::Protocol::UDP;

        disable_splithz = false;
        disable_dedup = false;
        use_reset_wait_strategy = false;

        peer_config.listen_port = client_config.listen_port + 3;
        peer_config.peer_port = client_config.listen_port + 3;
        peer_config.bind_addr = client_config.bind_addr;
        peer_config.protocol = client_config.protocol;
        peer_config.latitude = 0;
        peer_config.longitude = 0;



        char peer_id[200];
        gethostname(peer_id, sizeof(peer_id));
        peer_config.id = peer_id;

        cache_map_capacity = 20000;
        cache_max_buffers = 10;
        data_queue_size = 500;
        time_queue_ttl_default = 200;
        rx_queue_size = 200;

        help_msg.push_back("lapsRelay [help|-h]\n");
        help_msg.push_back("Environment Variables:");
    }

    void Config::print_help() {
        for (const auto &line: help_msg) {
            std::cout << line << std::endl;
        }
    }

    void Config::cfg_from_env()
    {
        env_value(debug, "LAPS_DEBUG");

        env_value(tls_cert_filename, "LAPS_TLS_CERT_FILENAME", "TLS certificate filename", "./server-cert.pem");
        env_value(tls_key_filename, "LAPS_TLS_KEY_FILENAME", "TLS private key filename", "./server-key.pem");

        if (tls_cert_filename.empty()) {
            tls_cert_filename = "./server-cert.pem";
            tls_key_filename = "./server-key.pem";
        }

        env_value(client_config.bind_addr, "LAPS_CLIENT_BIND_ADDR", "Client bind address", client_config.bind_addr);
        env_value(client_config.listen_port, "LAPS_CLIENT_PORT", "Client listening port",
                  std::to_string(client_config.listen_port));

        env_value(data_queue_size, "LAPS_CLIENT_QUEUE_SIZE", "Initial client queue size",
                  std::to_string(data_queue_size));

        env_value(time_queue_ttl_default, "LAPS_CLIENT_TQ_TTL", "Initial TTL used for time queues",
                  std::to_string(time_queue_ttl_default));

        env_value(cache_map_capacity, "LAPS_CACHE_MAP_CAPACITY", "Max number of objects in cache buffer", std::to_string(cache_map_capacity));
        env_value(cache_max_buffers, "LAPS_CACHE_MAX_BUFFERS", "Max number of buffers", std::to_string(cache_max_buffers));

        env_value(cache_expire_ms, "LAPS_CACHE_EXPIRE_MS", "Expire interval/age for cached objects", std::to_string(cache_expire_ms));
        if (cache_expire_ms < 500) {
            cache_expire_ms = 500;
        }

        env_value(disable_dedup, "LAPS_DISABLE_DEDUP", "Disable deduplication detection", "false");
        env_value(disable_splithz, "LAPS_DISABLE_SPLITHZ", "Disable split horizon detection", "false");
        env_value(use_reset_wait_strategy, "LAPS_RESET_WAIT", "Enables reset and wait strategy", "false");
        env_value(use_bbr, "LAPS_USE_BBR", "True to enable BBR, false to use NewReno", "true");

        env_value(cwin_min_kb, "LAPS_CWIN_MIN_KB", "Congestion control window minimum size", std::to_string(cwin_min_kb));

        peer_config.listen_port = client_config.listen_port + 3;
        peer_config.peer_port = client_config.listen_port + 3;

        env_value(peer_config.id, "LAPS_PEER_ID", "Local peer ID, must be unique", peer_config.id);
        env_value(peer_config.listen_port , "LAPS_PEER_LISTEN_PORT", "Peer listening port", std::to_string(peer_config.listen_port));

        env_value(peer_config.peer_port , "LAPS_PEER_PORT", "Peer connect port", std::to_string(peer_config.peer_port));


        env_value(peer_config.peers, "LAPS_PEERS", "Space or comma delimited peer IP or hostnames");
        env_value(peer_config.use_reliable, "LAPS_PEER_RELIABLE", "Peer connections use reliable transport", "true");
    }

    template<typename Value_t>
    void Config::env_value(Value_t& value, std::string&& var, std::string help_descr, std::string def_value)
    {
        if (not help_descr.empty()) {
            std::ostringstream os;
            os << "   " << std::setw(24) << std::left << var << std::right << std::setw(1) << " : " << help_descr;

            if (not def_value.empty()) {
                os << std::endl << std::setfill(' ') << std::setw(30) << "" << std::setw(1) << "Default: " << def_value;
            }

            help_msg.push_back(os.str());
        }

        const char* envVar = getenv(var.c_str());
        if (!envVar)
            return;

        if constexpr ( std::is_same_v<Value_t, uint16_t>
            || std::is_same_v<Value_t, int>) {
            value = atoi(envVar);
        }

        else if constexpr (std::is_same_v<Value_t, uint32_t>
                 || std::is_same_v<Value_t, unsigned int>
                 || std::is_same_v<Value_t, long> ) {
            value = atol(envVar);
        }

        else if constexpr (std::is_same_v<Value_t, uint64_t>
                 || std::is_same_v<Value_t, unsigned long>) {
            value = atoll(envVar);
        }

        else if constexpr (std::is_same_v<Value_t, std::string>) {
            value = envVar;
        }

        else if constexpr (std::is_same_v<Value_t, bool>) {
            value = std::string(envVar) == std::string("1") || std::string(envVar) == std::string("true") ? true : false;
        }

        else if constexpr (std::is_same_v<Value_t, std::vector<std::string>>) {
            value.clear();

            // A vector of strings is parsed as comma and/or whitespace delimited values
            std::string s = envVar;
            size_t end_pos = -1;
            size_t start_pos = 0;

            do {
                end_pos = s.find_first_of(" ,", end_pos+1);

                if (start_pos != end_pos)
                    value.push_back(s.substr(start_pos, end_pos - start_pos));

                start_pos = end_pos + 1;
            } while(end_pos != std::string::npos);
        }
    }

    void Config::init_logger()
    {
        if (debug)
        {
            logger->SetLogLevel(cantina::LogLevel::Debug);
        }
        else
        {
            logger->SetLogLevel(cantina::LogLevel::Info);
        }
    }
} // namespace laps
