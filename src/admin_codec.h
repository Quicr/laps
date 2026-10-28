// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <quicr/track_name.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace laps {
    /// Track name used for both directions of an admin RPC.
    inline constexpr std::string_view kAdminRpcTrackName = "rpc";

    /// Default subscribe-tracks base prefix. Slash separates tuple entries.
    inline constexpr std::string_view kDefaultAdminPrefix = "laps/admin";

    /// First admin RPC. Returns sizes of the relay's internal maps.
    inline constexpr std::string_view kAdminStateSizesMethod = "state";

    /// Peering sessions this relay has accepted or dialed, with RTT and forwarding counts.
    inline constexpr std::string_view kAdminPeersMethod = "peers";

    /// Latest connection and track metrics samples.
    inline constexpr std::string_view kAdminMetricsMethod = "metrics";

    /// Dial a peer, or remember the address when that peer is already accepted.
    inline constexpr std::string_view kAdminAddPeerMethod = "add-peer";

    /// Disconnect a peer and drop the forwarding state for it.
    inline constexpr std::string_view kAdminRemovePeerMethod = "remove-peer";

    /// Disconnect a peer and drop its forwarding state. A dialed peer is connected again.
    inline constexpr std::string_view kAdminRestartPeerMethod = "restart-peer";

    /// Admin RPC spoken by this build. Stays at 1 until the API is released.
    inline constexpr std::uint64_t kAdminApiVersion = 1;

    /// Connection handle of the relay's own subscribe-tracks interest. Not a QUIC connection.
    inline constexpr std::uint64_t kLocalAdminSubscriberConnection = static_cast<std::uint64_t>(-1);

    /**
     * @brief Namespace layout for one admin client talking to one relay
     *
     * @details MoQ allows one publisher per full track name, so each sender is encoded in the
     *      namespace. The relay subscribes to `base/relay`. The client publishes requests on
     *      `base/relay/client` and subscribes to replies on `base/client/relay`. The track name
     *      is `rpc` in both directions.
     */
    struct AdminTrackLayout
    {
        std::vector<std::string> subscribe_prefix;
        std::vector<std::string> request;
        std::vector<std::string> reply;
    };

    /// One cached track. oldest_group and newest_group are set only when the track holds a group.
    struct AdminCacheTrack
    {
        std::uint64_t track_alias{ 0 };
        std::string track_namespace;
        std::string track_name;
        std::uint64_t groups{ 0 };
        std::uint64_t objects{ 0 };
        std::uint64_t bytes{ 0 };
        bool has_groups{ false };
        std::uint64_t oldest_group{ 0 };
        std::uint64_t newest_group{ 0 };
    };

    /// Contents of the relay object cache.
    struct AdminCacheMetrics
    {
        bool enabled{ false };
        std::uint64_t duration_ms{ 0 };
        std::uint64_t interval_ms{ 0 };
        std::uint64_t tracks{ 0 };
        std::uint64_t groups{ 0 };
        std::uint64_t objects{ 0 };
        std::uint64_t bytes{ 0 };
        std::vector<AdminCacheTrack> entries;
    };

    /// Relay endpoint id, peering node id, and the time this process started.
    struct AdminRelayIdentity
    {
        std::string relay_id;
        std::string node_id;
        /// Unix time in microseconds when the relay started. 0 when unknown.
        std::uint64_t start_time_us{ 0 };
    };

    /// One admin reply to place in the printed results document.
    struct AdminResultCall
    {
        std::string method;
        std::string body;
    };

    /// Sizes of the relay maps reported by the state RPC.
    struct AdminStateSizes
    {
        std::uint64_t connections{ 0 };
        std::uint64_t subscriptions{ 0 };
        std::uint64_t publishes{ 0 };
        std::uint64_t publish_namespaces{ 0 };
        std::uint64_t subscribe_namespaces{ 0 };
        std::uint64_t subscribe_tracks{ 0 };
        std::uint64_t requests{ 0 };
        std::uint64_t subscribe_active{ 0 };
        std::uint64_t publish_subscribes_by_request{ 0 };
        std::uint64_t subscribe_alias_by_request{ 0 };
        std::uint64_t connection_metrics{ 0 };
        std::uint64_t fanout_namespaces{ 0 };
        std::uint64_t notified_namespaces{ 0 };
        std::uint64_t cache_tracks{ 0 };
        std::uint64_t track_rankings{ 0 };
        std::uint64_t peering_interests{ 0 };
        AdminCacheMetrics cache;
    };

    /// One peering session reported by the peers RPC.
    struct AdminPeer
    {
        std::uint64_t session_id{ 0 };
        std::string role;
        std::string status;
        std::string host;
        std::uint16_t port{ 0 };
        bool identified{ false };
        std::string node_id;
        std::string contact;
        std::string node_type;
        std::uint64_t srtt_us{ 0 };
        std::uint64_t connect_time_us{ 0 };
        std::uint64_t subscribe_tracks{ 0 };
        std::uint64_t publishes{ 0 };
    };

    /// Peers this relay has accepted or dialed.
    struct AdminPeers
    {
        std::vector<AdminPeer> peers;
    };

    /// host:port argument of add-peer, remove-peer, and restart-peer.
    struct AdminPeerEndpoint
    {
        std::string host;
        std::uint16_t port{ 0 };
    };

    /// Result of add-peer, remove-peer, or restart-peer.
    struct AdminPeerCommand
    {
        std::string host;
        std::uint16_t port{ 0 };
        std::string action;
        std::string message;
    };

    /// One JSON object exchanged on an admin RPC track.
    struct AdminRpcMessage
    {
        bool valid{ false };
        std::string error;
        std::string id;
        bool has_method{ false };
        std::string method;
        bool has_params{ false };
        std::string params_json;
        bool has_result{ false };
        std::string result_json;
        bool has_error{ false };
        std::string error_message;
        bool has_api_version{ false };
        std::uint64_t api_version{ 0 };
        bool has_relay_id{ false };
        std::string relay_id;
        bool has_node_id{ false };
        std::string node_id;
        bool has_start_time_us{ false };
        std::uint64_t start_time_us{ 0 };
        bool has_start_time{ false };
        std::string start_time;
    };

    /// Split a slash-delimited namespace. Empty segments are dropped.
    std::vector<std::string> SplitTrackNamespace(std::string_view value);

    std::string JoinTrackNamespace(const std::vector<std::string>& entries);

    /**
     * @brief Build the subscribe, request, and reply namespaces
     *
     * @param base_prefix     Slash-delimited base, such as laps/admin
     * @param relay_endpoint  Relay endpoint id. A slash is a tuple delimiter
     * @param client_endpoint Admin client endpoint id. A slash is a tuple delimiter
     * @param error           Set when the names are empty or the namespace would exceed 32 entries
     */
    std::optional<AdminTrackLayout> MakeAdminTrackLayout(std::string_view base_prefix,
                                                         std::string_view relay_endpoint,
                                                         std::string_view client_endpoint,
                                                         std::string& error);

    /// Entries after subscribe_prefix, when the namespace is a longer match. Empty when it is not.
    std::optional<std::vector<std::string>> AdminClientSuffix(const quicr::TrackNamespace& track_namespace,
                                                              const std::vector<std::string>& subscribe_prefix);

    quicr::FullTrackName AdminFullTrackName(const std::vector<std::string>& name_space);

    AdminRpcMessage ParseAdminMessage(std::string_view json);

    /// True when value is one JSON value and nothing follows it.
    bool IsJsonValue(std::string_view json, std::string& error);

    /// Indent one JSON value with two spaces. Empty when json is not a single value.
    std::optional<std::string> PrettyPrintJson(std::string_view json);

    std::string EncodeAdminRequest(std::string_view id,
                                   std::string_view method,
                                   std::optional<std::string_view> params_json = std::nullopt);

    std::string EncodeAdminResult(std::string_view id,
                                  const AdminRelayIdentity& identity,
                                  std::string_view result_json);

    std::string EncodeAdminError(std::string_view id, const AdminRelayIdentity& identity, std::string_view message);

    /**
     * @brief One JSON document for every reply from a command
     *
     * @details `api_version`, `relay_id`, `node_id`, and the relay start time are taken from the replies.
     *      Each call's `result` object is stored under `results` by method name, without that wrapper. A method
     *      that was called more than once uses `method#1`, `method#2`, and so on. `fallback_relay_id`
     *      is used when no reply carries a relay id.
     */
    std::string EncodeAdminResultsDocument(const std::vector<AdminResultCall>& calls,
                                           std::string_view fallback_relay_id);

    std::string EncodeAdminStateSizes(const AdminStateSizes& sizes);

    std::string EncodeAdminPeers(const AdminPeers& peers);

    /// `connections` and `tracks` are JSON objects already encoded, in dump order.
    std::string EncodeAdminMetrics(const std::vector<std::string>& connections,
                                   const std::vector<std::string>& tracks);

    /// Split a comma-delimited parameter argument. Tokens are trimmed. Empty tokens are kept.
    std::vector<std::string> SplitAdminParams(std::string_view text);

    /// Parse `host:port`. IPv6 addresses use `[addr]:port`.
    std::optional<AdminPeerEndpoint> ParseAdminPeerEndpoint(std::string_view text, std::string& error);

    /// Parse `{"host":"...","port":N}`.
    std::optional<AdminPeerEndpoint> ParseAdminPeerParams(std::string_view params_json, std::string& error);

    std::string EncodeAdminPeerParams(const AdminPeerEndpoint& endpoint);

    std::string EncodeAdminPeerCommand(const AdminPeerCommand& command);
} // namespace laps
