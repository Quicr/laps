
#include "config.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <string>
#include <unistd.h>

namespace laps {

    std::optional<quicr::TransportBackend> ParseTransportBackend(std::string_view name)
    {
        if (name == "msquic") {
            return quicr::TransportBackend::kMsQuic;
        }
        if (name == "picoquic") {
            return quicr::TransportBackend::kPicoQuic;
        }
        return std::nullopt;
    }

    Config::Config()
      : logger_(spdlog::stderr_color_mt("lapsRelay"))
      , quicr_logger_(std::make_shared<SpdlogLogger>(logger_))
      , tick_service_(std::make_shared<timeq::threaded_tick_service>())
    {
        InitDefaults();
        InitLogger();
    }

    void Config::InitDefaults()
    {
        char relay_id[200];
        gethostname(relay_id, sizeof(relay_id));
        relay_id_ = relay_id;
        metrics_namespace_ = "metrics/" + relay_id_;
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
