// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "relay_health_check_internal.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace laps {
namespace relay_health {

    std::string GetEnvOrDefault(const char* key, std::string fallback)
    {
        const auto value = std::getenv(key);
        if (value == nullptr || std::string_view(value).empty()) {
            return fallback;
        }
        return value;
    }

    std::chrono::milliseconds ParseTimeout(std::string_view value)
    {
        try {
            const auto text = std::string(value);
            std::size_t consumed = 0;
            const auto parsed = std::stoul(text, &consumed);
            if (consumed != text.size()) {
                throw std::runtime_error("timeout must be an integer number of milliseconds");
            }
            return std::chrono::milliseconds(parsed);
        } catch (const std::runtime_error&) {
            throw;
        } catch (const std::exception&) {
            throw std::runtime_error("timeout must be an integer number of milliseconds");
        }
    }

    std::vector<std::string> SplitNamespace(std::string_view value)
    {
        std::vector<std::string> parts;
        std::string current;
        for (const char ch : value) {
            if (ch == '/' || ch == ',') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }

        if (!current.empty()) {
            parts.push_back(current);
        }

        if (parts.empty()) {
            parts.emplace_back("health");
        }

        return parts;
    }

    std::vector<std::uint8_t> ToBytes(std::string_view value)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(value.size());
        for (const auto ch : value) {
            bytes.push_back(static_cast<std::uint8_t>(ch));
        }
        return bytes;
    }

    std::string MakeDefaultTrackName(std::uint64_t clock_value)
    {
        return "probe-" + std::to_string(clock_value);
    }

    void ApplyEnvironment(Options& options)
    {
        options.uri = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_URI", options.uri);
        options.name_space = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_NAMESPACE", options.name_space);
        options.message = GetEnvOrDefault("LIBQUICR_RELAY_HEALTH_MESSAGE", options.message);

        if (const auto name = std::getenv("LIBQUICR_RELAY_HEALTH_NAME"); name != nullptr && *name != '\0') {
            options.name = name;
        }

        if (const auto timeout = std::getenv("LIBQUICR_RELAY_HEALTH_TIMEOUT_MS");
            timeout != nullptr && *timeout != '\0') {
            options.timeout = ParseTimeout(timeout);
        }

        if (const auto debug = std::getenv("LIBQUICR_RELAY_HEALTH_DEBUG"); debug != nullptr && *debug != '\0') {
            options.debug = std::string_view(debug) == "1" || std::string_view(debug) == "true";
        }

        options.gateway_namespace =
          GetEnvOrDefault("LIBQUICR_GATEWAY_HEALTH_NAMESPACE", options.gateway_namespace);
        options.gateway_name = GetEnvOrDefault("LIBQUICR_GATEWAY_HEALTH_NAME", options.gateway_name);
        options.gateway_message = GetEnvOrDefault("LIBQUICR_GATEWAY_HEALTH_MESSAGE", options.gateway_message);

        if (const auto enabled = std::getenv("LIBQUICR_GATEWAY_HEALTH_ENABLED");
            enabled != nullptr && *enabled != '\0') {
            options.gateway_enabled = std::string_view(enabled) == "1" || std::string_view(enabled) == "true";
        }
    }

    void PrintUsage(std::ostream& out, const char* program)
    {
        out << "Usage: " << program
            << " [--uri URI] [--timeout-ms MS] [--namespace NS] [--name NAME] [--message TEXT] [--debug]\n"
            << "       [--gateway] [--gateway-namespace NS] [--gateway-name NAME] [--gateway-message TEXT]\n"
            << "\n"
            << "Environment overrides:\n"
            << "  LIBQUICR_RELAY_HEALTH_URI\n"
            << "  LIBQUICR_RELAY_HEALTH_TIMEOUT_MS\n"
            << "  LIBQUICR_RELAY_HEALTH_NAMESPACE\n"
            << "  LIBQUICR_RELAY_HEALTH_NAME\n"
            << "  LIBQUICR_RELAY_HEALTH_MESSAGE\n"
            << "  LIBQUICR_RELAY_HEALTH_DEBUG\n"
            << "  LIBQUICR_GATEWAY_HEALTH_ENABLED    (\"1\" or \"true\" enables the gateway subscribe probe)\n"
            << "  LIBQUICR_GATEWAY_HEALTH_NAMESPACE  (default: frontline.m10x.org/health)\n"
            << "  LIBQUICR_GATEWAY_HEALTH_NAME       (default: health_check)\n"
            << "  LIBQUICR_GATEWAY_HEALTH_MESSAGE    (default: pong)\n";
    }

    Options ParseOptions(int argc, char* argv[])
    {
        Options options;
        ApplyEnvironment(options);

        auto require_value = [&](int& index, const std::string_view option) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            ++index;
            return argv[index];
        };

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                PrintUsage(std::cerr, argv[0]);
                std::exit(EXIT_SUCCESS);
            }
            if (arg == "--uri") {
                options.uri = require_value(i, arg);
            } else if (arg == "--timeout-ms") {
                options.timeout = ParseTimeout(require_value(i, arg));
            } else if (arg == "--namespace") {
                options.name_space = require_value(i, arg);
            } else if (arg == "--name") {
                options.name = require_value(i, arg);
            } else if (arg == "--message") {
                options.message = require_value(i, arg);
            } else if (arg == "--debug") {
                options.debug = true;
            } else if (arg == "--gateway") {
                options.gateway_enabled = true;
            } else if (arg == "--gateway-namespace") {
                options.gateway_namespace = require_value(i, arg);
            } else if (arg == "--gateway-name") {
                options.gateway_name = require_value(i, arg);
            } else if (arg == "--gateway-message") {
                options.gateway_message = require_value(i, arg);
            } else {
                throw std::runtime_error("unknown option: " + std::string(arg));
            }
        }

        return options;
    }

}
}
