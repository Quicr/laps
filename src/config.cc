
#include "config.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <string>
#include <unistd.h>

namespace laps {

    Config::Config()
      : logger_(spdlog::stderr_color_mt("lapsRelay"))
      , tick_service_(std::make_shared<quicr::ThreadedTickService>())
    {
        InitDefaults();
        InitLogger();
    }

    void Config::InitDefaults()
    {
        char relay_id[200];
        gethostname(relay_id, sizeof(relay_id));
        relay_id_ = relay_id;
        object_ttl_ = kDefaultObjectTtl;
    }

    void Config::InitLogger()
    {
        if (debug) {
            logger_->set_level(spdlog::level::debug);
        } else {
            logger_->set_level(spdlog::level::info);
        }
    }
} // namespace laps