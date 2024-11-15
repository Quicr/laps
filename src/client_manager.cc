// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include "client_manager.h"
#include "config.h"
#include "publish_handler.h"
#include "subscribe_handler.h"

namespace laps {
    ClientManager::ClientManager(State& state, const quicr::ServerConfig& cfg, peering::PeerManager& peer_manager)
      : quicr::Server(cfg)
      , state_(state)
      , peer_manager_(peer_manager)
    {
    }

    void ClientManager::NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                              const ConnectionRemoteInfo& remote)
    {
        SPDLOG_INFO("New connection handle {0} accepted from {1}:{2}", connection_handle, remote.ip, remote.port);
    }

    void ClientManager::UnannounceReceived(quicr::ConnectionHandle connection_handle,
                                           const quicr::TrackNamespace& track_namespace)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_DEBUG("Received unannounce from connection handle: {0} for namespace hash: {1}, removing all tracks "
                     "associated with namespace",
                     connection_handle,
                     th.track_namespace_hash);

        for (auto track_alias : state_.announce_active[{ th.track_namespace_hash, connection_handle }]) {
            auto ptd = state_.pub_subscribes[{ track_alias, connection_handle }];
            if (ptd != nullptr) {
                SPDLOG_INFO("Received unannounce from connection handle: {0} for namespace hash: {1}, removing "
                            "track alias: {2}",
                            connection_handle,
                            th.track_namespace_hash,
                            track_alias);

                UnsubscribeTrack(connection_handle, ptd);
            }
            state_.pub_subscribes.erase({ track_alias, connection_handle });
        }

        state_.announce_active.erase({ th.track_namespace_hash, connection_handle });
        peer_manager_.ClientUnannounce({ track_namespace, {}, th.track_fullname_hash });
    }

    void ClientManager::PurgePublishState(quicr::ConnectionHandle connection_handle)
    {
        std::lock_guard<std::mutex> _(state_.state_mutex);

        std::vector<std::pair<quicr::messages::TrackAlias, quicr::ConnectionHandle>> pub_subs;
        for (const auto& [key, _] : state_.pub_subscribes) {
            if (key.second == connection_handle) {
                pub_subs.push_back(key);
            }
        }

        for (const auto& remove_key : pub_subs) {
            state_.pub_subscribes.erase(remove_key);
            SPDLOG_DEBUG(
              "Purge publish state for track_alias: {0} connection handle: {1}", remove_key.first, remove_key.second);
        }

        std::vector<std::pair<quicr::TrackNamespaceHash, quicr::ConnectionHandle>> anno_remove_list;
        for (const auto& [key, _] : state_.announce_active) {
            if (key.second == connection_handle) {
                anno_remove_list.push_back(key);
            }
        }

        for (const auto& remove_key : anno_remove_list) {
            state_.announce_active.erase(remove_key);
            SPDLOG_DEBUG("Purge publish state for namespace_hash: {0} connection handle: {1}",
                         remove_key.first,
                         remove_key.second);
        }
    }
    void ClientManager::AnnounceReceived(quicr::ConnectionHandle connection_handle,
                                         const quicr::TrackNamespace& track_namespace,
                                         const quicr::PublishAnnounceAttributes& attrs)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_INFO("Received announce from connection handle: {0} for namespace_hash: {1}",
                    connection_handle,
                    th.track_namespace_hash);

        // Add to state if not exist
        auto it = state_.announce_active.find({ th.track_namespace_hash, connection_handle });
        if (it != state_.announce_active.end()) {
            /*
             * @note Connection handle maybe reused. This requires replacing an existing entry if it's duplicate
             */
            PurgePublishState(connection_handle);
        } else {
            peer_manager_.ClientAnnounce({ track_namespace, {}, th.track_fullname_hash }, attrs, false);
        }

        AnnounceResponse announce_response;
        announce_response.reason_code = quicr::Server::AnnounceResponse::ReasonCode::kOk;
        ResolveAnnounce(connection_handle, track_namespace, announce_response);

        auto& anno_tracks = state_.announce_active[{ th.track_namespace_hash, connection_handle }];

        // Check if there are any subscribes. If so, send subscribe to announce for all tracks matching namespace
        // for (const auto& [key, who] : state_.subscribe_active)
        for (auto it = state_.subscribe_active_.lower_bound({ th.track_namespace_hash, 0 });
             it != state_.subscribe_active_.end();
             it++) {
            const auto& [key, who] = *it;

            if (key.first != th.track_namespace_hash)
                break;

            if (who.size()) { // Have subscribes
                auto& a_who = *who.begin();
                if (anno_tracks.find(a_who.track_alias) == anno_tracks.end()) {
                    SPDLOG_INFO("Sending subscribe to announcer connection handle: {0} subscribe track_alias: {1}",
                                connection_handle,
                                a_who.track_alias);

                    anno_tracks.insert(a_who.track_alias); // Add track to state

                    const auto& sub_info_it = state_.subscribes.find({ a_who.track_alias, a_who.connection_handle });
                    if (sub_info_it == state_.subscribes.end()) {
                        continue;
                    }

                    const auto& sub_ftn = sub_info_it->second.track_full_name;

                    // TODO(tievens): Don't really like passing self to subscribe handler, see about fixing this
                    auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(
                      sub_ftn, 0, quicr::messages::GroupOrder::kOriginalPublisherOrder, *this);

                    SubscribeTrack(connection_handle, sub_track_handler);
                    state_.pub_subscribes[{ a_who.track_alias, connection_handle }] = sub_track_handler;
                }
            }
        }
    }

    void ClientManager::ConnectionStatusChanged(quicr::ConnectionHandle connection_handle, ConnectionStatus status)
    {
        switch (status) {
            case ConnectionStatus::kConnected:
                SPDLOG_DEBUG("Connection ready; connection_handle: {0} ", connection_handle);
                return;
            case ConnectionStatus::kConnecting:
                return;

            case ConnectionStatus::kNotConnected:
                SPDLOG_DEBUG("Connection not connected; connection_handle: {0} ", connection_handle);
                break;
            case ConnectionStatus::kClosedByRemote:
                SPDLOG_DEBUG("Connection closed by remote; connection_handle: {0} ", connection_handle);
                break;
            case ConnectionStatus::kIdleTimeout:
                SPDLOG_DEBUG("Connection idle timeout; connection_handle: {0} ", connection_handle);
                break;
        }

        // Clean up subscribe states
        std::vector<std::pair<quicr::ConnectionHandle, quicr::messages::SubscribeId>> unsub_list;
        for (auto it = state_.subscribe_alias_sub_id.lower_bound({ connection_handle, 0 });
             it != state_.subscribe_alias_sub_id.end();
             it++) {
            const auto& [key, _] = *it;
            if (key.first != connection_handle)
                break;

            unsub_list.push_back(key);
        }

        for (auto& key : unsub_list) {
            UnsubscribeReceived(key.first, key.second);
        }

        // Cleanup publish states
        PurgePublishState(connection_handle);
    }

    quicr::Server::ClientSetupResponse ClientManager::ClientSetupReceived(
      quicr::ConnectionHandle,
      const quicr::ClientSetupAttributes& client_setup_attributes)
    {
        ClientSetupResponse client_setup_response;

        SPDLOG_INFO("Client setup received from endpoint_id: {0}", client_setup_attributes.endpoint_id);

        return client_setup_response;
    }

    void ClientManager::UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t subscribe_id)
    {
        SPDLOG_INFO("Unsubscribe connection handle: {0} subscribe_id: {1}", connection_handle, subscribe_id);

        const auto ta_it = state_.subscribe_alias_sub_id.find({ connection_handle, subscribe_id });
        if (ta_it == state_.subscribe_alias_sub_id.end()) {
            SPDLOG_WARN("Unable to find track alias for connection handle: {0} subscribe_id: {1}",
                        connection_handle,
                        subscribe_id);
            return;
        }

        std::lock_guard<std::mutex> _(state_.state_mutex);

        auto track_alias = ta_it->second;
        state_.subscribe_alias_sub_id.erase(ta_it);

        const auto sub_it = state_.subscribes.find({ track_alias, connection_handle });
        if (sub_it == state_.subscribes.end()) {
            SPDLOG_DEBUG("Unsubscribe unable to find track handler for connection handle: {0} subscribe_id: {1}",
                         connection_handle,
                         subscribe_id);
            return;
        }

        auto ftn = sub_it->second.track_full_name;
        auto th = quicr::TrackHash(sub_it->second.track_full_name);

        state_.subscribes.erase(sub_it);

        auto& sub_active_list = state_.subscribe_active_[{ th.track_namespace_hash, th.track_name_hash }];
        sub_active_list.erase(State::SubscribeInfo{ connection_handle, subscribe_id, th.track_fullname_hash });

        if (sub_active_list.empty()) {
            state_.subscribe_active_.erase({ th.track_namespace_hash, th.track_name_hash });
        }

        // Are there any other subscribers?
        bool unsub_pub{ true };
        for (auto it = state_.subscribes.lower_bound({ track_alias, 0 }); it != state_.subscribes.end(); ++it) {
            if (it->first.first == track_alias) {
                unsub_pub = false;
                break;
            }
        }

        if (unsub_pub) {
            SPDLOG_INFO("No subscribers left, unsubscribe publisher track_alias: {0}", track_alias);

            peer_manager_.ClientUnsubscribe(ftn);

            std::vector<std::pair<quicr::messages::TrackAlias, quicr::ConnectionHandle>> remove_sub_pub;
            for (auto it = state_.pub_subscribes.lower_bound({ th.track_namespace_hash, 0 });
                 it != state_.pub_subscribes.end();
                 ++it) {
                const auto& [key, sub_to_pub_handler] = *it;

                if (key.first != th.track_fullname_hash)
                    break;

                SPDLOG_INFO("Unsubscribe to announcer conn_id: {0} subscribe track_alias: {1}",
                            key.second,
                            th.track_fullname_hash);

                UnsubscribeTrack(key.second, sub_to_pub_handler);
                remove_sub_pub.push_back(key);
            }

            for (const auto& key : remove_sub_pub) {
                state_.pub_subscribes.erase(key);
            }
        }
    }
    void ClientManager::SubscribeReceived(quicr::ConnectionHandle connection_handle,
                                          uint64_t subscribe_id,
                                          [[maybe_unused]] uint64_t proposed_track_alias,
                                          const quicr::FullTrackName& track_full_name,
                                          const quicr::SubscribeAttributes& attrs)
    {
        auto th = quicr::TrackHash(track_full_name);

        SPDLOG_INFO("New subscribe connection handle: {0} subscribe_id: {1} track alias: {2} priority: {3}",
                    connection_handle,
                    subscribe_id,
                    th.track_fullname_hash,
                    attrs.priority);

        ProcessSubscribe(connection_handle, subscribe_id, th, track_full_name, attrs);
    }

    void ClientManager::ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                         uint64_t subscribe_id,
                                         const quicr::TrackHash& th,
                                         const quicr::FullTrackName& track_full_name,
                                         const quicr::SubscribeAttributes& attrs)
    {
        auto is_peer{ false };
        if (connection_handle == 0 && subscribe_id == 0) {
            is_peer = true;
            SPDLOG_DEBUG(
              "Processing peer subscribe track alias: {} priority: {}", th.track_fullname_hash, attrs.priority);
        }

        else {
            SPDLOG_DEBUG("Processing subscribe connection handle: {0} subscribe_id: {1} track alias: {2} priority: {3}",
                         connection_handle,
                         subscribe_id,
                         th.track_fullname_hash,
                         attrs.priority);

            state_.subscribe_alias_sub_id[{ connection_handle, subscribe_id }] = th.track_fullname_hash;

            // record subscribe as active from this subscriber
            state_.subscribe_active_[{ th.track_namespace_hash, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle, subscribe_id, th.track_fullname_hash });
            state_.subscribe_alias_sub_id[{ connection_handle, subscribe_id }] = th.track_fullname_hash;

            const auto [_, is_new] = state_.subscribes.try_emplace(
              { th.track_fullname_hash, connection_handle },
              State::SubscribePublishHandlerInfo{
                track_full_name, th.track_fullname_hash, subscribe_id, attrs.priority, attrs.group_order, nullptr });

            if (is_new) {
                quicr::messages::MoqSubscribe sub;
                sub.group_order = attrs.group_order;
                sub.priority = attrs.priority;
                sub.subscribe_id = subscribe_id;
                sub.track_alias = th.track_fullname_hash;
                sub.track_namespace = track_full_name.name_space;
                sub.track_name = track_full_name.name;

                quicr::Bytes sub_data;
                sub_data << sub;

                // TODO(tievens): Clean up total hack to accommodate encode using operator overload that adds common
                // header
                auto mt_sz = quicr::UintVar::Size(*quicr::UintVar(sub_data).begin());
                auto len_sz = quicr::UintVar::Size(*quicr::UintVar(sub_data).begin() + mt_sz);
                sub_data.erase(sub_data.begin(), sub_data.begin() + mt_sz + len_sz);

                peer_manager_.ClientSubscribe(track_full_name, attrs, sub_data, false);
            }
        }

        // Subscribe to announcer if announcer is active
        for (auto it = state_.announce_active.lower_bound({ th.track_namespace_hash, 0 });
             it != state_.announce_active.end();
             ++it) {
            auto& [key, track_aliases] = *it;

            if (key.first != th.track_namespace_hash)
                break;

            SPDLOG_INFO("Sending subscribe to announcer connection handler: {0} subscribe track_alias: {1}",
                        key.second,
                        th.track_fullname_hash);

            track_aliases.insert(th.track_fullname_hash); // Add track alias to state

            auto sub_track_h =
              std::make_shared<SubscribeTrackHandler>(track_full_name,
                                                      0 /* use zero to indicate to use publisher priority */,
                                                      quicr::messages::GroupOrder::kAscending,
                                                      *this);
            SubscribeTrack(key.second, sub_track_h);
            state_.pub_subscribes[{ th.track_fullname_hash, key.second }] = sub_track_h;
        }
    }

    void ClientManager::MetricsSampled(const quicr::ConnectionHandle connection_handle,
                                       const quicr::ConnectionMetrics& metrics)
    {
        SPDLOG_DEBUG("Metrics connection handle: {0}"
                     " rtt_us: {1}"
                     " srtt_us: {2}"
                     " rate_bps: {3}"
                     " lost pkts: {4}",
                     connection_handle,
                     metrics.quic.rtt_us.max,
                     metrics.quic.srtt_us.max,
                     metrics.quic.tx_rate_bps.max,
                     metrics.quic.tx_lost_pkts);
    }
}