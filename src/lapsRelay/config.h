#pragma once

#include <quicr/quicr_common.h>

#include "logger.h"

namespace laps {
    class Config
    {
      public:
        Logger* logger;                        /// Local source logger reference

        // Client Manager
        struct clientConfig {
            std::string bind_addr;            /// Local client bind address, defaults to 127.0.0.1
            uint16_t listen_port;             /// Local client listening port, defaults to 33434

            quicr::RelayInfo::Protocol protocol;
        } client_config;


        bool disable_splithz;                 /// Disable split horizon
        bool disable_dedup;                   /// Disable Deduplication

        std::string tls_cert_filename;
        std::string tls_key_filename;

        uint16_t data_queue_size;
        uint16_t time_queue_ttl_default;

        // Peering Manager Config parameters
        struct peerConfig {
            std::string bind_addr;          /// bind address, defaults to client bind address
            uint16_t listen_port;           /// Peering listening port
            quicr::RelayInfo::Protocol protocol;

            std::vector<std::string> peers; /// Default peers to connect to

            std::vector<quicr::Namespace> sub_namespaces;       /// Initial namespaces to subscribe to

            std::string id;                 /// Peering ID of this peering manager
            double longitude;               /// This peer manager longitude
            double latitude;                /// This peer latitude
        } peer_config;

        // Cache
        unsigned int cache_max_buffers;       /// Max number of cache buffers
        unsigned int cache_map_capacity;      /// Max capacity for cache map

        // constructor
        Config();

      private:
        /**
         * @brief Initialize thread safe logger
         *
         * @return True on error, false if no error
         */
        bool init_logger();

        template<typename Value_t>
        void env_value(Value_t& vtype, std::string&& var);

        void init_defaults();
        void cfg_from_env();
    };

} // namespace laps
