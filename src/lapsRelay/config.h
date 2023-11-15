#pragma once

#include <cantina/logger.h>
#include <quicr/namespace.h>
#include <quicr/quicr_common.h>
#include <list>

namespace laps {
    class Config
    {
      public:
        cantina::LoggerPointer logger;        /// Local source logger reference

        // Client Manager
        struct clientConfig {
            std::string bind_addr;            /// Local client bind address, defaults to 127.0.0.1
            uint16_t listen_port;             /// Local client listening port, defaults to 33434

            quicr::RelayInfo::Protocol protocol;
        } client_config;


        bool debug { false };                 /// Debug logging/code
        bool disable_splithz;                 /// Disable split horizon
        bool disable_dedup;                   /// Disable Deduplication

        std::string tls_cert_filename;
        std::string tls_key_filename;

        uint16_t data_queue_size;
        uint16_t time_queue_ttl_default;
        uint16_t rx_queue_size;
        uint16_t cwin_min_kb { 128 };                  /// QUIC CWIN minimum KB size
        uint32_t client_wifi_shadow_rtt_us{ 60000 };   /// QUIC wifi shadow rtt microseconds

        // Peering Manager Config parameters
        struct peerConfig {
            std::string bind_addr;          /// bind address, defaults to client bind address
            uint16_t listen_port;           /// Peering listening port
            uint16_t peer_port;             /// Peering destination port to connect to
            quicr::RelayInfo::Protocol protocol;

            bool use_reliable { true };     /// Indicates if using reliable or not

            std::vector<std::string> peers; /// Default peers to connect to

            std::string id;                 /// Peering ID of this peering manager
            double longitude;               /// This peer manager longitude
            double latitude;                /// This peer latitude

            uint32_t wifi_shadow_rtt_us{ 60000 };   /// QUIC wifi shadow rtt microseconds
        } peer_config;

        // Cache
        unsigned int cache_max_buffers;       /// Max number of cache buffers
        unsigned int cache_map_capacity;      /// Max capacity for cache map
        unsigned int cache_expire_ms { 5000 };         /// Expire cache interval in milliseconds

        // constructor
        Config();

        void print_help();

      private:
        /**
         * @brief Initialize thread safe logger
         *
         * @return Nothing
         */
        void init_logger();

        template<typename Value_t>
        void env_value(Value_t& vtype, std::string&& var, std::string help_descr={}, std::string def_value={});

        void init_defaults();
        void cfg_from_env();

        std::list<std::string> help_msg;
    };

} // namespace laps
