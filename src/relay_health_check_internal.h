// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace laps {
namespace relay_health {

    struct Options
    {
        std::string uri{ "moq://laps-relay:12345/relay" };
        std::chrono::milliseconds timeout{ 5000 };
        std::string name_space{ "libquicr/health" };
        std::optional<std::string> name;
        std::string message{ "libquicr relay health check" };
        bool debug{ false };

        bool gateway_enabled{ false };
        std::string gateway_namespace{ "frontline.m10x.org/health" };
        std::string gateway_name{ "health_check" };
        std::string gateway_message{ "pong" };
    };

    std::string GetEnvOrDefault(const char* key, std::string fallback);

    std::chrono::milliseconds ParseTimeout(std::string_view value);

    std::vector<std::string> SplitNamespace(std::string_view value);

    std::vector<std::uint8_t> ToBytes(std::string_view value);

    std::string MakeDefaultTrackName(std::uint64_t clock_value);

    void ApplyEnvironment(Options& options);

    Options ParseOptions(int argc, char* argv[]);

    void PrintUsage(std::ostream& out, const char* program);

}
}
