#pragma once

#include "version_config.h"

#include <list>
#include <moq/config.h>
#include <spdlog/spdlog.h>

namespace laps {
    class Config
    {
      public:
        std::shared_ptr<spdlog::logger> logger_; // Local source logger reference

        moq::ServerConfig server_config;

        bool debug{ false }; /// Debug logging/code
        bool use_reset_wait_strategy{ false };

        std::string relay_id_;
        std::string tls_cert_filename_;
        std::string tls_key_filename_;
        std::string qlog_path_;

        uint16_t data_queue_size_;
        uint16_t time_queue_ttl_default_;
        uint16_t rx_queue_size_;
        uint16_t cwin_min_kb_{ 128 }; /// QUIC CWIN minimum KB size
        uint8_t priority_limit_bypass_{ 0 };

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