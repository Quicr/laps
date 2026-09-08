// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <quicr/log.h>

#include <spdlog/spdlog.h>

#include <memory>
#include <source_location>
#include <string_view>
#include <utility>

namespace laps {
    /**
     * @brief quicr::Logger backed by an spdlog logger
     *
     * @details libquicr logs through its own abstract logger interface, so relay logging is bridged to the
     *      spdlog logger the rest of the relay uses. Levels are not cached here, so changing the level on the
     *      spdlog logger also changes what libquicr logs.
     */
    class SpdlogLogger : public quicr::Logger
    {
      public:
        explicit SpdlogLogger(std::shared_ptr<spdlog::logger> logger)
          : logger_(std::move(logger))
        {
        }

        void SetLevel(Level max_level) override { logger_->set_level(ToSpdlogLevel(max_level)); }

        bool ShouldLog(Level level) const noexcept override { return logger_->should_log(ToSpdlogLevel(level)); }

        /// @note quicr::Logger has already formatted the message, so this only forwards it
        void Log(Level level, std::string_view msg, std::source_location location) override
        {
            logger_->log(
              spdlog::source_loc(location.file_name(), static_cast<int>(location.line()), location.function_name()),
              ToSpdlogLevel(level),
              msg);
        }

      private:
        static spdlog::level::level_enum ToSpdlogLevel(Level level) noexcept
        {
            switch (level) {
                case Level::Trace:
                    return spdlog::level::trace;
                case Level::Debug:
                    return spdlog::level::debug;
                case Level::Info:
                    return spdlog::level::info;
                case Level::Warn:
                    return spdlog::level::warn;
                case Level::Error:
                    return spdlog::level::err;
                case Level::Critical:
                    return spdlog::level::critical;
                case Level::Off:
                    return spdlog::level::off;
            }

            return spdlog::level::info;
        }

        std::shared_ptr<spdlog::logger> logger_;
    };
} // namespace laps
