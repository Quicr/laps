
#include "config.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <string>
#include <unistd.h>

namespace laps {

    Config::Config()
      : logger_(spdlog::stderr_color_mt("lapsRelay"))
    {
        InitDefaults();
        InitLogger();
    }

    void Config::InitDefaults()
    {
        server_config.server_bind_ip = "127.0.0.1";
        server_config.server_port = 33434;

        use_reset_wait_strategy = false;


        char relay_id[200];
        gethostname(relay_id, sizeof(relay_id));
        relay_id_ = relay_id;

        data_queue_size_ = 500;
        time_queue_ttl_default_ = 500;
        rx_queue_size_ = 200;
        priority_limit_bypass_ = 0;
    }

    void Config::InitLogger()
    {
        if (debug)
        {
            logger_->set_level(spdlog::level::debug);
        }
        else
        {
            logger_->set_level(spdlog::level::info);
        }
    }
} // namespace laps