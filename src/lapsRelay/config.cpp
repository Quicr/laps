
#include "config.h"
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace laps {

    Config::Config()
      : logger(NULL)
    {

        init_logger();

        init_defaults();
        cfg_from_env();
    }

    void Config::init_defaults()
    {
        client_config.bind_addr = "127.0.0.1";
        client_config.listen_port = 33434;
        client_config.protocol = quicr::RelayInfo::Protocol::UDP;

        disable_splithz = false;
        disable_dedup = false;

        peer_config.listen_port = client_config.listen_port;
        peer_config.peer_port = peer_config.listen_port + 3;
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
    }

    void Config::cfg_from_env()
    {
        env_value(tls_cert_filename, "LAPS_TLS_CERT_FILENAME");
        env_value(tls_key_filename, "LAPS_TLS_KEY_FILENAME");

        if (tls_cert_filename.empty()) {
            tls_cert_filename = "./server-cert.pem";
            tls_key_filename = "./server-key.pem";
        }

        env_value(client_config.bind_addr, "LAPS_CLIENT_BIND_ADDR");
        env_value(client_config.listen_port, "LAPS_CLIENT_PORT");

        env_value(data_queue_size, "LAPS_CLIENT_QUEUE_SIZE");
        env_value(time_queue_ttl_default, "LAPS_CLIENT_TQ_TTL");

        env_value(cache_map_capacity, "LAPS_CACHE_MAP_CAPACITY");
        env_value(cache_max_buffers, "LAPS_CACHE_MAX_BUFFERS");

        env_value(disable_dedup, "LAPS_DISABLE_DEDUP");
        env_value(disable_splithz, "LAPS_DISABLE_SPLITHZ");

        env_value(peer_config.id, "LAPS_PEER_ID");
        env_value(peer_config.peer_port , "LAPS_PEER_PORT");
        env_value(peer_config.listen_port , "LAPS_PEER_LISTEN_PORT");
        env_value(peer_config.peers, "LAPS_PEERS");
        env_value(peer_config.use_reliable, "LAPS_PEER_RELIABLE");
    }

    template<typename Value_t>
    void Config::env_value(Value_t& value, std::string&& var)
    {
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

    bool Config::init_logger()
    {
        try {
            if (logger != NULL)
                delete logger;

            logger = new Logger(NULL, NULL);

        } catch (char const* str) {
            std::cout << "Failed to open log file for read/write : " << str << std::endl;
            return true;
        }

        // Set up defaults for logging
        logger->setWidthFilename(15);
        logger->setWidthFunction(18);

        if (std::getenv("LAPS_DEBUG"))
            logger->enableDebug();

        return false;
    }
} // namespace laps