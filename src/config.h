#pragma once

#include "peering/messages/node_info.h"
#include "version_config.h"

#include <spdlog/sinks/stdout_color_sinks-inl.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <list>
#include <optional>

namespace laps {
#define LOGGER config_.logger_

    constexpr uint16_t kDefaultClientPort = 33435;
    constexpr uint16_t kDefaultPeerPort = 33434;
    constexpr uint64_t kDefaultPeerCheckIntervalMs = 5'000;
    constexpr uint32_t kDefaultPeerInitQueueSize = 5'000;
    constexpr uint32_t kDefaultObjectTtl = 5'000;
    constexpr uint8_t kDefaultPriority = 10;
    constexpr uint32_t kDefaultCacheTimeQueueMaxDuration = 10'000;
    constexpr uint32_t kDefaultCacheTimeQueueObjectTtl = 6'000;
    constexpr uint32_t kDefaultSubscriptionRefreshIntervalMs = 500;
    constexpr uint32_t kFetchUpstreamMaxWaitMs = 2000;

    class Config
    {
      public:
        std::shared_ptr<spdlog::logger> logger_ = spdlog::stderr_color_mt("LAPS");

        std::string bind_ip;
        std::uint16_t port;

        bool debug{ false }; /// Debug logging/code
        bool use_reset_wait_strategy{ false };
        bool detached_subs{ false };
        bool allow_self{ false };

        std::string relay_id_;
        std::string tls_cert_filename_;
        std::string tls_key_filename_;
        std::string qlog_path_;
        uint32_t object_ttl_;
        uint32_t sub_dampen_ms_;

        peering::NodeType node_type{ peering::NodeType::kEdge }; /// Node type of the relay

        std::optional<std::uint64_t> cache_key = std::nullopt;
        std::size_t cache_duration_ms = 60000;

        struct Peering
        {
            uint16_t listening_port;                             /// Peer listening port
            std::vector<std::pair<std::string, uint16_t>> peers; /// Peer host/ip and port

            uint64_t check_interval_ms{ kDefaultPeerCheckIntervalMs }; /// Peer check interval in milliseconds
            uint32_t init_queue_size{ kDefaultPeerInitQueueSize };

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