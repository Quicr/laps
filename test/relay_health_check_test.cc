// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <doctest/doctest.h>

#include "relay_health_check_internal.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

    using laps::relay_health::ApplyEnvironment;
    using laps::relay_health::GetEnvOrDefault;
    using laps::relay_health::MakeDefaultTrackName;
    using laps::relay_health::Options;
    using laps::relay_health::ParseOptions;
    using laps::relay_health::ParseTimeout;
    using laps::relay_health::SplitNamespace;
    using laps::relay_health::ToBytes;

    class ScopedEnv
    {
      public:
        explicit ScopedEnv(std::vector<const char*> keys)
        {
            keys_.reserve(keys.size());
            for (const auto* key : keys) {
                Snapshot snapshot;
                snapshot.key = key;
                if (const auto* current = std::getenv(key); current != nullptr) {
                    snapshot.had_value = true;
                    snapshot.original = current;
                }
                keys_.push_back(std::move(snapshot));
                ::unsetenv(key);
            }
        }

        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

        ~ScopedEnv()
        {
            for (const auto& snapshot : keys_) {
                if (snapshot.had_value) {
                    ::setenv(snapshot.key, snapshot.original.c_str(), 1);
                } else {
                    ::unsetenv(snapshot.key);
                }
            }
        }

        void Set(const char* key, const char* value) { ::setenv(key, value, 1); }

      private:
        struct Snapshot
        {
            const char* key{ nullptr };
            bool had_value{ false };
            std::string original;
        };

        std::vector<Snapshot> keys_;
    };

    const std::vector<const char*> kAllEnvKeys = {
        "LIBQUICR_RELAY_HEALTH_URI",       "LIBQUICR_RELAY_HEALTH_TIMEOUT_MS",  "LIBQUICR_RELAY_HEALTH_NAMESPACE",
        "LIBQUICR_RELAY_HEALTH_NAME",      "LIBQUICR_RELAY_HEALTH_MESSAGE",     "LIBQUICR_RELAY_HEALTH_DEBUG",
        "LIBQUICR_GATEWAY_HEALTH_ENABLED", "LIBQUICR_GATEWAY_HEALTH_NAMESPACE", "LIBQUICR_GATEWAY_HEALTH_NAME",
        "LIBQUICR_GATEWAY_HEALTH_MESSAGE",
    };

}

TEST_CASE("SplitNamespace: relay default")
{
    const auto parts = SplitNamespace("libquicr/health");
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "libquicr");
    CHECK(parts[1] == "health");
}

TEST_CASE("SplitNamespace: gateway default matches [frontline.m10x.org, health]")
{
    const auto parts = SplitNamespace("frontline.m10x.org/health");
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "frontline.m10x.org");
    CHECK(parts[1] == "health");
}

TEST_CASE("SplitNamespace: comma separator")
{
    const auto parts = SplitNamespace("a,b,c");
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "a");
    CHECK(parts[1] == "b");
    CHECK(parts[2] == "c");
}

TEST_CASE("SplitNamespace: leading/trailing slashes are ignored")
{
    const auto parts = SplitNamespace("/leading/slash/");
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "leading");
    CHECK(parts[1] == "slash");
}

TEST_CASE("SplitNamespace: empty string falls back to health")
{
    const auto parts = SplitNamespace("");
    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == "health");
}

TEST_CASE("SplitNamespace: single token")
{
    const auto parts = SplitNamespace("single");
    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == "single");
}

TEST_CASE("ToBytes: ping payload")
{
    const auto bytes = ToBytes("ping");
    REQUIRE(bytes.size() == 4);
    CHECK(bytes[0] == static_cast<std::uint8_t>('p'));
    CHECK(bytes[1] == static_cast<std::uint8_t>('i'));
    CHECK(bytes[2] == static_cast<std::uint8_t>('n'));
    CHECK(bytes[3] == static_cast<std::uint8_t>('g'));
}

TEST_CASE("ToBytes: empty")
{
    CHECK(ToBytes("").empty());
}

TEST_CASE("ParseTimeout: valid integer")
{
    CHECK(ParseTimeout("5000") == std::chrono::milliseconds(5000));
    CHECK(ParseTimeout("0") == std::chrono::milliseconds(0));
}

TEST_CASE("ParseTimeout: non-numeric rejected")
{
    CHECK_THROWS_AS(ParseTimeout("not-a-number"), std::runtime_error);
}

TEST_CASE("ParseTimeout: trailing unit rejected")
{
    CHECK_THROWS_AS(ParseTimeout("5000ms"), std::runtime_error);
}

TEST_CASE("GetEnvOrDefault: unset returns fallback")
{
    ScopedEnv env({ "LAPS_TEST_UNSET_KEY" });
    CHECK(GetEnvOrDefault("LAPS_TEST_UNSET_KEY", "fallback") == "fallback");
}

TEST_CASE("GetEnvOrDefault: set returns value")
{
    ScopedEnv env({ "LAPS_TEST_SET_KEY" });
    env.Set("LAPS_TEST_SET_KEY", "actual");
    CHECK(GetEnvOrDefault("LAPS_TEST_SET_KEY", "fallback") == "actual");
}

TEST_CASE("GetEnvOrDefault: empty value returns fallback")
{
    ScopedEnv env({ "LAPS_TEST_EMPTY_KEY" });
    env.Set("LAPS_TEST_EMPTY_KEY", "");
    CHECK(GetEnvOrDefault("LAPS_TEST_EMPTY_KEY", "fallback") == "fallback");
}

TEST_CASE("ApplyEnvironment: defaults preserved when nothing is set (relay regression baseline)")
{
    ScopedEnv env(kAllEnvKeys);

    Options options;
    ApplyEnvironment(options);

    CHECK(options.uri == "moq://laps-relay:12345/relay");
    CHECK(options.timeout == std::chrono::milliseconds(5000));
    CHECK(options.name_space == "libquicr/health");
    CHECK_FALSE(options.name.has_value());
    CHECK(options.message == "libquicr relay health check");
    CHECK_FALSE(options.debug);

    CHECK_FALSE(options.gateway_enabled);
    CHECK(options.gateway_namespace == "frontline.m10x.org/health");
    CHECK(options.gateway_name == "health_check");
    CHECK(options.gateway_message == "pong");
}

TEST_CASE("ApplyEnvironment: relay overrides picked up")
{
    ScopedEnv env(kAllEnvKeys);
    env.Set("LIBQUICR_RELAY_HEALTH_URI", "moq://other:1234/relay");
    env.Set("LIBQUICR_RELAY_HEALTH_NAMESPACE", "custom/relay/ns");
    env.Set("LIBQUICR_RELAY_HEALTH_NAME", "fixed-name");
    env.Set("LIBQUICR_RELAY_HEALTH_MESSAGE", "custom message");
    env.Set("LIBQUICR_RELAY_HEALTH_TIMEOUT_MS", "1234");
    env.Set("LIBQUICR_RELAY_HEALTH_DEBUG", "1");

    Options options;
    ApplyEnvironment(options);

    CHECK(options.uri == "moq://other:1234/relay");
    CHECK(options.name_space == "custom/relay/ns");
    REQUIRE(options.name.has_value());
    CHECK(*options.name == "fixed-name");
    CHECK(options.message == "custom message");
    CHECK(options.timeout == std::chrono::milliseconds(1234));
    CHECK(options.debug);
}

TEST_CASE("ApplyEnvironment: gateway defaults are stable")
{
    ScopedEnv env(kAllEnvKeys);

    Options options;
    ApplyEnvironment(options);

    CHECK_FALSE(options.gateway_enabled);
    CHECK(options.gateway_namespace == "frontline.m10x.org/health");
    CHECK(options.gateway_name == "health_check");
    CHECK(options.gateway_message == "pong");

    const auto ns = SplitNamespace(options.gateway_namespace);
    REQUIRE(ns.size() == 2);
    CHECK(ns[0] == "frontline.m10x.org");
    CHECK(ns[1] == "health");
}

TEST_CASE("ApplyEnvironment: gateway enable toggle only accepts 1/true")
{
    for (const char* truthy : { "1", "true" }) {
        ScopedEnv env(kAllEnvKeys);
        env.Set("LIBQUICR_GATEWAY_HEALTH_ENABLED", truthy);
        Options options;
        ApplyEnvironment(options);
        CHECK(options.gateway_enabled);
    }

    for (const char* falsy : { "0", "false", "yes", "TRUE", "on" }) {
        ScopedEnv env(kAllEnvKeys);
        env.Set("LIBQUICR_GATEWAY_HEALTH_ENABLED", falsy);
        Options options;
        ApplyEnvironment(options);
        CHECK_FALSE(options.gateway_enabled);
    }
}

TEST_CASE("ApplyEnvironment: gateway overrides do not implicitly enable")
{
    ScopedEnv env(kAllEnvKeys);
    env.Set("LIBQUICR_GATEWAY_HEALTH_NAMESPACE", "other.example.org/health");
    env.Set("LIBQUICR_GATEWAY_HEALTH_NAME", "other_check");
    env.Set("LIBQUICR_GATEWAY_HEALTH_MESSAGE", "beacon");

    Options options;
    ApplyEnvironment(options);

    CHECK_FALSE(options.gateway_enabled);
    CHECK(options.gateway_namespace == "other.example.org/health");
    CHECK(options.gateway_name == "other_check");
    CHECK(options.gateway_message == "beacon");
}

TEST_CASE("ParseOptions: CLI overrides env for relay probe")
{
    ScopedEnv env(kAllEnvKeys);
    env.Set("LIBQUICR_RELAY_HEALTH_URI", "moq://env-uri:1/relay");

    const char* argv[] = { "relay_health_check", "--uri", "moq://cli-uri:2/relay" };
    const int argc = sizeof(argv) / sizeof(argv[0]);

    const auto options = ParseOptions(argc, const_cast<char**>(argv));
    CHECK(options.uri == "moq://cli-uri:2/relay");
}

TEST_CASE("ParseOptions: --gateway enables probe with default namespace/name/message")
{
    ScopedEnv env(kAllEnvKeys);

    const char* argv[] = { "relay_health_check", "--gateway" };
    const int argc = sizeof(argv) / sizeof(argv[0]);

    const auto options = ParseOptions(argc, const_cast<char**>(argv));
    CHECK(options.gateway_enabled);
    CHECK(options.gateway_namespace == "frontline.m10x.org/health");
    CHECK(options.gateway_name == "health_check");
    CHECK(options.gateway_message == "pong");
}

TEST_CASE("ParseOptions: gateway CLI flags override env")
{
    ScopedEnv env(kAllEnvKeys);
    env.Set("LIBQUICR_GATEWAY_HEALTH_NAMESPACE", "env/ns");
    env.Set("LIBQUICR_GATEWAY_HEALTH_NAME", "env-name");
    env.Set("LIBQUICR_GATEWAY_HEALTH_MESSAGE", "env-msg");

    const char* argv[] = { "relay_health_check", "--gateway", "--gateway-namespace", "cli/ns",
                           "--gateway-name",     "cli-name",  "--gateway-message",   "cli-msg" };
    const int argc = sizeof(argv) / sizeof(argv[0]);

    const auto options = ParseOptions(argc, const_cast<char**>(argv));
    CHECK(options.gateway_enabled);
    CHECK(options.gateway_namespace == "cli/ns");
    CHECK(options.gateway_name == "cli-name");
    CHECK(options.gateway_message == "cli-msg");
}

TEST_CASE("ParseOptions: unknown flag rejected")
{
    ScopedEnv env(kAllEnvKeys);
    const char* argv[] = { "relay_health_check", "--nope" };
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CHECK_THROWS_AS(ParseOptions(argc, const_cast<char**>(argv)), std::runtime_error);
}

TEST_CASE("ParseOptions: --uri missing value rejected")
{
    ScopedEnv env(kAllEnvKeys);
    const char* argv[] = { "relay_health_check", "--uri" };
    const int argc = sizeof(argv) / sizeof(argv[0]);
    CHECK_THROWS_AS(ParseOptions(argc, const_cast<char**>(argv)), std::runtime_error);
}

TEST_CASE("MakeDefaultTrackName: deterministic for a given clock value")
{
    CHECK(MakeDefaultTrackName(0) == "probe-0");
    CHECK(MakeDefaultTrackName(42) == "probe-42");
}
