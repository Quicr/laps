#pragma once

#include "peering/messages/node_info.h"
#include "spdlog/sinks/stdout_color_sinks-inl.h"
#include "version_config.h"

#include <list>
#include <quicr/config.h>
#include <quicr/detail/tick_service.h>
#include <spdlog/spdlog.h>

namespace laps {
#define LOGGER config_.logger_
    constexpr uint16_t kDefaultClientPort = 33435;
    constexpr uint16_t kDefaultPeerPort = 33434;
    constexpr uint64_t kDefaultPeerCheckIntervalMs = 5'000;
    constexpr uint32_t kDefaultPeerInitQueueSize = 5'000;
    constexpr uint32_t kDefaultPeerTtlExpiryMs = 5'000;
    constexpr uint32_t kDefaultObjectTtl = 2'000;

    class Config
    {
      public:
        std::shared_ptr<spdlog::logger> logger_ = spdlog::stderr_color_mt("LAPS");

        quicr::ServerConfig server_config;

        bool debug{ false }; /// Debug logging/code
        bool use_reset_wait_strategy{ false };

        std::string relay_id_;
        std::string tls_cert_filename_;
        std::string tls_key_filename_;
        std::string qlog_path_;
        uint32_t object_ttl_;

        peering::NodeType node_type{ peering::NodeType::kEdge }; /// Node type of the relay

        std::shared_ptr<quicr::ThreadedTickService> tick_service_;
        std::optional<std::uint64_t> cache_key = std::nullopt;

        struct Peering
        {
            uint16_t listening_port;                             /// Peer listening port
            std::vector<std::pair<std::string, uint16_t>> peers; /// Peer host/ip and port

            uint64_t check_interval_ms{ kDefaultPeerCheckIntervalMs }; /// Peer check interval in milliseconds
            uint32_t init_queue_size{ kDefaultPeerInitQueueSize };
            uint32_t max_ttl_expiry_ms{ kDefaultPeerTtlExpiryMs };

        } peering;

        // constructor
        Config();

        std::string Version() { return LAPS_VERSION; }

      private:
        /**
         * @brief Initialize thread safe logger
         *
         * @return Nothing
         */
        void InitLogger();

        void InitDefaults();
    };

} // namespace laps