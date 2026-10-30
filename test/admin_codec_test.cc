// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <doctest/doctest.h>

#include "admin_codec.h"

#include <string>

TEST_CASE("admin namespace layout")
{
    std::string error;
    const auto layout = laps::MakeAdminTrackLayout("laps/admin", "relay-b", "admin-client-xyz", error);
    REQUIRE(layout.has_value());
    CHECK(layout->subscribe_prefix == std::vector<std::string>{ "laps", "admin", "relay-b" });
    CHECK(layout->request == std::vector<std::string>{ "laps", "admin", "relay-b", "admin-client-xyz" });
    CHECK(layout->reply == std::vector<std::string>{ "laps", "admin", "admin-client-xyz", "relay-b" });

    const quicr::TrackNamespace request(layout->request);
    const auto suffix = laps::AdminClientSuffix(request, layout->subscribe_prefix);
    REQUIRE(suffix.has_value());
    CHECK(*suffix == std::vector<std::string>{ "admin-client-xyz" });

    const quicr::TrackNamespace reply(layout->reply);
    CHECK_FALSE(laps::AdminClientSuffix(reply, layout->subscribe_prefix).has_value());

    const auto split_ids = laps::MakeAdminTrackLayout("laps/admin/", "relay/b", "admin/client", error);
    REQUIRE(split_ids.has_value());
    CHECK(split_ids->subscribe_prefix == std::vector<std::string>{ "laps", "admin", "relay", "b" });
    CHECK(split_ids->request == std::vector<std::string>{ "laps", "admin", "relay", "b", "admin", "client" });
    CHECK(split_ids->reply == std::vector<std::string>{ "laps", "admin", "admin", "client", "relay", "b" });

    const auto same_endpoint = laps::MakeAdminTrackLayout("laps/admin", "relay-b", "relay-b", error);
    CHECK_FALSE(same_endpoint.has_value());
    CHECK(error.find("begin with the target relay endpoint") != std::string::npos);

    const auto client_extends_relay = laps::MakeAdminTrackLayout("laps/admin", "relay-b", "relay-b/extra", error);
    CHECK_FALSE(client_extends_relay.has_value());

    const auto string_prefix = laps::MakeAdminTrackLayout("laps/admin", "relay-b", "relay-b-extra", error);
    REQUIRE(string_prefix.has_value());
    CHECK_FALSE(laps::AdminClientSuffix(quicr::TrackNamespace(string_prefix->reply), string_prefix->subscribe_prefix)
                  .has_value());

    const auto partial_tuple = laps::MakeAdminTrackLayout("laps/admin", "relay/b", "relay/extra", error);
    REQUIRE(partial_tuple.has_value());
    CHECK_FALSE(laps::AdminClientSuffix(quicr::TrackNamespace(partial_tuple->reply), partial_tuple->subscribe_prefix)
                  .has_value());
}

TEST_CASE("admin rpc json")
{
    const auto request = laps::ParseAdminMessage(R"({"id":"7","method":"state","params":{"verbose":true}})");
    CHECK(request.valid);
    CHECK(request.id == "7");
    CHECK(request.method == "state");
    CHECK(request.has_params);
    CHECK(request.params_json == R"({"verbose":true})");

    const auto numeric_id = laps::ParseAdminMessage(R"({"id":42,"method":"state"})");
    CHECK(numeric_id.valid);
    CHECK(numeric_id.id == "42");
    CHECK(numeric_id.id_is_number);
    CHECK_FALSE(numeric_id.has_params);
    CHECK_FALSE(request.id_is_number);

    laps::AdminRelayIdentity identity;
    identity.relay_id = "relay-b";
    identity.node_id = "0.0:0.1001";
    identity.start_time_us = 1720000000000000;
    const auto numeric_reply = laps::EncodeAdminResult(numeric_id, identity, R"({"accepted_connections":0})");
    CHECK(numeric_reply.find(R"("id":42,)") != std::string::npos);
    CHECK(numeric_reply.find(R"("id":"42")") == std::string::npos);
    const auto numeric_again = laps::ParseAdminMessage(numeric_reply);
    CHECK(numeric_again.id_is_number);
    CHECK(numeric_again.id == "42");

    const auto numeric_error = laps::EncodeAdminError(numeric_id, identity, "unknown method");
    CHECK(numeric_error.find(R"("id":42,)") != std::string::npos);

    const auto string_wins = laps::ParseAdminMessage(R"({"id":1,"id":"abc","method":"state"})");
    CHECK(string_wins.valid);
    CHECK(string_wins.id == "abc");
    CHECK_FALSE(string_wins.id_is_number);
    const auto string_reply = laps::EncodeAdminResult(string_wins, identity, R"({"accepted_connections":0})");
    CHECK(string_reply.find(R"("id":"abc",)") != std::string::npos);

    const auto number_wins = laps::ParseAdminMessage(R"({"id":"abc","id":7,"method":"state"})");
    CHECK(number_wins.valid);
    CHECK(number_wins.id == "7");
    CHECK(number_wins.id_is_number);

    const auto fractional = laps::ParseAdminMessage(R"({"id":42.5,"method":"state"})");
    CHECK(fractional.valid);
    CHECK(fractional.id_is_number);
    CHECK(fractional.id == "42.5");

    const auto emoji = laps::ParseAdminMessage(R"({"id":"\uD83D\uDE00","method":"state"})");
    CHECK(emoji.valid);
    CHECK(emoji.id == "\xF0\x9F\x98\x80");
    CHECK(laps::ParseAdminMessage(R"({"method":"state","note":"\uD83D\uDE00"})").valid);
    CHECK(laps::ParseAdminMessage(R"({"method":"add-peer","params":{"host":"\uD83D\uDE00"}})").valid);
    CHECK(laps::PrettyPrintJson(R"("\uD83D\uDE00")").has_value());

    const auto unpaired = laps::ParseAdminMessage(R"({"id":"\uD83D","method":"state"})");
    CHECK_FALSE(unpaired.valid);
    CHECK(unpaired.error.find("surrogate") != std::string::npos);
    CHECK_FALSE(laps::ParseAdminMessage(R"({"id":"\uDE00","method":"state"})").valid);
    CHECK_FALSE(laps::ParseAdminMessage(R"({"method":"state","note":"\uD83D\u0041"})").valid);
    CHECK_FALSE(laps::PrettyPrintJson(R"("\uD83D")").has_value());

    const auto broken = laps::ParseAdminMessage("{");
    CHECK_FALSE(broken.valid);

    std::string nested(32, '[');
    nested += "1";
    nested.append(32, ']');
    const auto deep_enough = laps::ParseAdminMessage("{\"method\":\"state\",\"extra\":" + nested + "}");
    CHECK(deep_enough.valid);

    nested.insert(nested.begin(), '[');
    nested.push_back(']');
    const auto too_deep = laps::ParseAdminMessage("{\"method\":\"state\",\"extra\":" + nested + "}");
    CHECK_FALSE(too_deep.valid);
    CHECK(too_deep.error.find("nesting") != std::string::npos);
    CHECK_FALSE(laps::PrettyPrintJson(nested).has_value());

    laps::AdminStateSizes sizes;
    sizes.accepted_connections = 2;
    sizes.total_connections = 3;
    sizes.subscriptions = 4;
    sizes.publishes = 3;
    sizes.publish_namespaces = 1;
    sizes.subscribe_namespaces = 5;
    sizes.subscribe_tracks = 1;
    const auto encoded = laps::EncodeAdminResult("7", identity, laps::EncodeAdminStateSizes(sizes));
    const auto response = laps::ParseAdminMessage(encoded);
    CHECK(response.valid);
    CHECK(response.id == "7");
    CHECK(response.has_result);
    CHECK(response.has_api_version);
    CHECK(response.api_version == laps::kAdminApiVersion);
    CHECK(response.has_relay_id);
    CHECK(response.relay_id == "relay-b");
    CHECK(response.has_node_id);
    CHECK(response.node_id == "0.0:0.1001");
    CHECK(response.has_start_time_us);
    CHECK(response.start_time_us == 1720000000000000);
    CHECK(response.has_start_time);
    CHECK(response.start_time == "2024-07-03 09:46:40");
    CHECK(encoded.find("\"api_version\":" + std::to_string(laps::kAdminApiVersion)) != std::string::npos);
    CHECK(response.result_json.find("\"relay_id\"") == std::string::npos);
    CHECK(response.result_json.find("\"accepted_connections\":2") != std::string::npos);
    CHECK(response.result_json.find("\"total_connections\":3") != std::string::npos);
    CHECK(response.result_json.find("\"subscribe_namespaces\":5") != std::string::npos);

    sizes.cache.enabled = true;
    sizes.cache.duration_ms = 60000;
    sizes.cache.interval_ms = 1000;
    sizes.cache.tracks = 1;
    sizes.cache.groups = 2;
    sizes.cache.objects = 3;
    sizes.cache.bytes = 10;
    laps::AdminCacheTrack cached;
    cached.track_alias = 99;
    cached.track_namespace = "media/live";
    cached.track_name = "video";
    cached.groups = 2;
    cached.objects = 3;
    cached.bytes = 10;
    cached.has_groups = true;
    cached.oldest_group = 4;
    cached.newest_group = 5;
    sizes.cache.entries.push_back(cached);
    const auto with_cache = laps::EncodeAdminStateSizes(sizes);
    CHECK(with_cache.find("\"cache\":{\"enabled\":true,\"duration_ms\":60000,\"interval_ms\":1000,\"tracks\":1,"
                          "\"groups\":2,\"objects\":3,\"bytes\":10,\"entries\":[") != std::string::npos);
    CHECK(with_cache.find("\"track_namespace\":\"media/live\"") != std::string::npos);
    CHECK(with_cache.find("\"track_name\":\"video\"") != std::string::npos);
    CHECK(with_cache.find("\"oldest_group\":4,\"newest_group\":5") != std::string::npos);
    CHECK(laps::ParseAdminMessage(laps::EncodeAdminResult("7", identity, with_cache)).valid);

    laps::AdminPeers peers;
    laps::AdminPeer accepted;
    accepted.session_id = 11;
    accepted.role = "accepted";
    accepted.status = "connected";
    accepted.host = "192.0.2.10";
    accepted.port = 33434;
    accepted.identified = true;
    accepted.node_id = "0.0:0.42";
    accepted.contact = "relay-a";
    accepted.node_type = "edge";
    accepted.srtt_us = 1500;
    accepted.connect_time_us = 1720000000000000;
    accepted.subscribe_tracks = 4;
    accepted.publishes = 2;
    laps::AdminPeer dialed;
    dialed.session_id = 12;
    dialed.role = "dialed";
    dialed.status = "connecting";
    dialed.host = "192.0.2.20";
    dialed.port = 33434;
    dialed.srtt_us = 0;
    peers.peers.push_back(accepted);
    peers.peers.push_back(dialed);
    const auto peer_json = laps::EncodeAdminPeers(peers);
    CHECK(peer_json.find("\"role\":\"accepted\"") != std::string::npos);
    CHECK(peer_json.find("\"role\":\"dialed\"") != std::string::npos);
    CHECK(peer_json.find("\"contact\":\"relay-a\"") != std::string::npos);
    CHECK(peer_json.find("\"node_type\":\"edge\"") != std::string::npos);
    CHECK(peer_json.find("\"metrics\":{\"srtt_us\":1500,\"connect_time_us\":1720000000000000,"
                         "\"connect_time\":\"2024-07-03 09:46:40\",\"subscribe_tracks\":4,\"publishes\":2}") !=
          std::string::npos);
    CHECK(peer_json.find("\"metrics\":{\"srtt_us\":0,\"connect_time_us\":0,\"connect_time\":\"\",\"subscribe_tracks\":"
                         "0,\"publishes\":0}") != std::string::npos);
    CHECK(peer_json.find("192.0.2.20") != std::string::npos);
    CHECK(peer_json.find("\"node_id\":\"0.0:0.42\"") != std::string::npos);
    const auto dialed_pos = peer_json.find("\"role\":\"dialed\"");
    CHECK(peer_json.find("\"node_type\"", dialed_pos) == std::string::npos);
    CHECK(laps::ParseAdminMessage(laps::EncodeAdminResult("9", identity, peer_json)).valid);

    const auto failure = laps::ParseAdminMessage(laps::EncodeAdminError("7", identity, "unknown method"));
    CHECK(failure.valid);
    CHECK(failure.has_error);
    CHECK(failure.has_api_version);
    CHECK(failure.api_version == laps::kAdminApiVersion);
    CHECK(failure.relay_id == "relay-b");
    CHECK(failure.node_id == "0.0:0.1001");
    CHECK(failure.error_message == "unknown method");

    std::string json_error;
    CHECK(laps::IsJsonValue("{}", json_error));
    CHECK_FALSE(laps::IsJsonValue("{", json_error));

    const auto pretty = laps::PrettyPrintJson(encoded);
    REQUIRE(pretty.has_value());
    CHECK(pretty->find("\n  \"id\": \"7\"") != std::string::npos);
    CHECK(pretty->find("\n  \"relay_id\": \"relay-b\"") != std::string::npos);
    CHECK(pretty->find("\n  \"node_id\": \"0.0:0.1001\"") != std::string::npos);
    CHECK(pretty->find("\n    \"accepted_connections\": 2") != std::string::npos);
    CHECK(pretty->find("\n    \"total_connections\": 3") != std::string::npos);
    CHECK(laps::ParseAdminMessage(*pretty).valid);
    CHECK(laps::PrettyPrintJson("{}") == "{}");
    CHECK(laps::PrettyPrintJson("[1,true]") == "[\n  1,\n  true\n]");
    CHECK_FALSE(laps::PrettyPrintJson("{").has_value());
    CHECK_FALSE(laps::PrettyPrintJson("1 2").has_value());

    std::vector<laps::AdminResultCall> calls;
    calls.push_back({ "state", encoded });
    calls.push_back({ "peers", laps::EncodeAdminResult("9", identity, peer_json) });
    calls.push_back(
      { "add-peer", laps::EncodeAdminResult("10", identity, "{\"host\":\"192.0.2.10\",\"port\":33434}") });
    calls.push_back({ "add-peer", laps::EncodeAdminError("11", identity, "duplicate peer 192.0.2.10:33434") });
    const auto document = laps::EncodeAdminResultsDocument(calls, "fallback");
    CHECK(document.find("\"api_version\":" + std::to_string(laps::kAdminApiVersion)) != std::string::npos);
    CHECK(document.find("\"relay_id\":\"relay-b\"") != std::string::npos);
    CHECK(document.find("\"node_id\":\"0.0:0.1001\"") != std::string::npos);
    CHECK(document.find("\"start_time_us\":1720000000000000") != std::string::npos);
    CHECK(document.find("\"start_time\":\"2024-07-03 09:46:40\"") != std::string::npos);
    CHECK(document.find("\"results\":{\"state\":{\"accepted_connections\":2") != std::string::npos);
    CHECK(document.find("\"peers\":{\"peers\":[") != std::string::npos);
    CHECK(document.find("\"add-peer#1\":{\"host\":\"192.0.2.10\",\"port\":33434}") != std::string::npos);
    CHECK(document.find("\"add-peer#2\":{\"error\":{\"message\":\"duplicate peer 192.0.2.10:33434\"}}") !=
          std::string::npos);
    CHECK(document.find("\"result\":") == std::string::npos);
    CHECK(document.find("fallback") == std::string::npos);
    CHECK(laps::PrettyPrintJson(document).has_value());

    const auto legacy = laps::EncodeAdminResultsDocument(
      { { "state", R"({"id":"1","result":{"relay_id":"legacy","connections":1}})" } }, "setup-id");
    CHECK(legacy.find("\"relay_id\":\"legacy\"") != std::string::npos);
    CHECK(legacy.find("\"node_id\"") == std::string::npos);
    CHECK(legacy.find("\"start_time\"") == std::string::npos);
    CHECK(legacy.find("\"results\":{\"state\":{\"connections\":1}}") != std::string::npos);
}

TEST_CASE("admin peer command params")
{
    const auto parts = laps::SplitAdminParams(" 192.0.2.10:33434, 192.0.2.11:33435 ");
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "192.0.2.10:33434");
    CHECK(parts[1] == "192.0.2.11:33435");

    std::string error;
    const auto endpoint = laps::ParseAdminPeerEndpoint("192.0.2.10:33434", error);
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->host == "192.0.2.10");
    CHECK(endpoint->port == 33434);

    const auto v6 = laps::ParseAdminPeerEndpoint("[2001:db8::1]:33434", error);
    REQUIRE(v6.has_value());
    CHECK(v6->host == "2001:db8::1");
    CHECK(v6->port == 33434);
    CHECK_FALSE(v6->metric.has_value());
    CHECK_FALSE(laps::ParseAdminPeerEndpoint("192.0.2.10", error).has_value());
    CHECK_FALSE(laps::ParseAdminPeerEndpoint("192.0.2.10:0", error).has_value());

    const auto with_metric = laps::ParseAdminPeerEndpoint("192.0.2.10:33434:250", error);
    REQUIRE(with_metric.has_value());
    CHECK(*with_metric->metric == 250);
    const auto v6_metric = laps::ParseAdminPeerEndpoint("[2001:db8::1]:33434:100", error);
    REQUIRE(v6_metric.has_value());
    CHECK(v6_metric->host == "2001:db8::1");
    CHECK(*v6_metric->metric == 100);
    CHECK_FALSE(laps::ParseAdminPeerEndpoint("192.0.2.10:33434", error, true).has_value());
    CHECK_FALSE(laps::ParseAdminPeerEndpoint("192.0.2.10:33434:-1", error).has_value());

    const auto encoded = laps::EncodeAdminPeerParams(*endpoint);
    const auto parsed = laps::ParseAdminPeerParams(encoded, error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->host == "192.0.2.10");
    CHECK(parsed->port == 33434);
    CHECK_FALSE(parsed->metric.has_value());

    const auto metric_json = laps::EncodeAdminPeerParams(*with_metric);
    const auto parsed_metric = laps::ParseAdminPeerParams(metric_json, error);
    REQUIRE(parsed_metric.has_value());
    CHECK(*parsed_metric->metric == 250);

    laps::AdminPeerCommand command;
    command.host = "192.0.2.10";
    command.port = 33434;
    command.action = "added";
    command.message = "Added peer 192.0.2.10:33434 but it is already accepted as inbound";
    const auto command_json = laps::EncodeAdminPeerCommand(command);
    CHECK(command_json.find("\"action\":\"added\"") != std::string::npos);
    CHECK(command_json.find("already accepted as inbound") != std::string::npos);
    laps::AdminRelayIdentity identity;
    identity.relay_id = "relay-b";
    identity.node_id = "0.0:0.1001";
    CHECK(laps::ParseAdminMessage(laps::EncodeAdminResult("9", identity, command_json)).valid);
}

TEST_CASE("admin metrics dump")
{
    const auto dump = laps::EncodeAdminMetrics(
      { R"({"type":"connection","connection_handle":10})", R"({"type":"connection","connection_handle":2})" },
      { R"({"type":"publish","track_namespace":"media/live","track_name":"video"})",
        R"({"type":"subscribe","track_namespace":"media/live","track_name":"video"})" });
    CHECK(
      dump ==
      R"({"connections":[{"type":"connection","connection_handle":10},{"type":"connection","connection_handle":2}],"tracks":[{"type":"publish","track_namespace":"media/live","track_name":"video"},{"type":"subscribe","track_namespace":"media/live","track_name":"video"}]})");
    CHECK(laps::EncodeAdminMetrics({}, {}) == R"({"connections":[],"tracks":[]})");
    CHECK(laps::PrettyPrintJson(dump).has_value());
}

TEST_CASE("admin track and namespace lists")
{
    const auto tracks =
      laps::EncodeAdminTrackList("subscriptions", { { 11, "media/live", "video" }, { 10, "media/\"a\"", "name" } });
    CHECK(
      tracks ==
      R"({"subscriptions":[{"connection_handle":11,"track_namespace":"media/live","track_name":"video"},{"connection_handle":10,"track_namespace":"media/\"a\"","track_name":"name"}]})");
    CHECK(laps::EncodeAdminTrackList("publishes", {}) == R"({"publishes":[]})");

    laps::AdminNamespaceEntry announced;
    announced.connection_handle = 10;
    announced.track_namespace = "media/live";
    announced.has_tracks = true;
    announced.tracks.push_back({ 99, "video" });
    announced.tracks.push_back({ 100, "" });
    laps::AdminNamespaceEntry local;
    local.connection_handle = laps::kLocalAdminSubscriberConnection;
    local.track_namespace = "laps/admin/relay-b";
    local.local = true;
    const auto namespaces = laps::EncodeAdminNamespaceList("publish_namespaces", { announced });
    CHECK(namespaces.find("\"tracks\":[{\"track_alias\":99,\"track_name\":\"video\"},{\"track_alias\":100,"
                          "\"track_name\":\"\"}]") != std::string::npos);
    const auto prefixes = laps::EncodeAdminNamespaceList("subscribe_tracks", { local });
    CHECK(
      prefixes ==
      R"({"subscribe_tracks":[{"connection_handle":18446744073709551615,"local":true,"track_namespace":"laps/admin/relay-b"}]})");
    CHECK(laps::EncodeAdminNamespaceList("subscribe_namespaces", {}) == R"({"subscribe_namespaces":[]})");
    CHECK(laps::PrettyPrintJson(tracks).has_value());
    CHECK(laps::PrettyPrintJson(prefixes).has_value());

    std::string raw_namespace = "media/";
    raw_namespace.push_back(static_cast<char>(0xFF));
    const std::string raw_name(1, static_cast<char>(0x80));
    const auto raw_bytes = laps::EncodeAdminTrackList("publishes", { { 1, raw_namespace, raw_name } });
    CHECK(raw_bytes.find("\\u00ff") != std::string::npos);
    CHECK(raw_bytes.find("\\u0080") != std::string::npos);
    CHECK(laps::PrettyPrintJson(raw_bytes).has_value());

    const auto utf8 = laps::EncodeAdminTrackList("publishes", { { 1, "ns", "vid\xc3\xa9o" } });
    CHECK(utf8.find("vid\xc3\xa9o") != std::string::npos);
    CHECK(laps::PrettyPrintJson(utf8).has_value());
}
