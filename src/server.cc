// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include "config.h"
#include "publish_handler.h"
#include "server.h"
#include "subscribe_handler.h"

namespace laps {
    LapsServer::LapsServer(State& state, const quicr::ServerConfig& cfg)
      : quicr::Server(cfg)
      , state_(state)
    {
    }

    void LapsServer::NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                           const ConnectionRemoteInfo& remote)
    {
        SPDLOG_INFO("New connection handle {0} accepted from {1}:{2}", connection_handle, remote.ip, remote.port);
    }

    void LapsServer::UnannounceReceived(quicr::ConnectionHandle connection_handle,
                                        const quicr::TrackNamespace& track_namespace)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_DEBUG("Received unannounce from connection handle: {0} for namespace hash: {1}, removing all tracks "
                     "associated with namespace",
                     connection_handle,
                     th.track_namespace_hash);

        for (auto track_alias : state_.announce_active[th.track_namespace_hash][connection_handle]) {
            auto ptd = state_.pub_subscribes[track_alias][connection_handle];
            if (ptd != nullptr) {
                SPDLOG_INFO("Received unannounce from connection handle: {0} for namespace hash: {1}, removing "
                            "track alias: {2}",
                            connection_handle,
                            th.track_namespace_hash,
                            track_alias);

                UnsubscribeTrack(connection_handle, ptd);
            }
            state_.pub_subscribes[track_alias].erase(connection_handle);
            if (state_.pub_subscribes[track_alias].empty()) {
                state_.pub_subscribes.erase(track_alias);
            }
        }

        state_.announce_active[th.track_namespace_hash].erase(connection_handle);
        if (state_.announce_active[th.track_namespace_hash].empty()) {
            state_.announce_active.erase(th.track_namespace_hash);
        }
    }

    void LapsServer::AnnounceReceived(quicr::ConnectionHandle connection_handle,
                                      const quicr::TrackNamespace& track_namespace,
                                      const quicr::PublishAnnounceAttributes&)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_INFO("Received announce from connection handle: {0} for namespace_hash: {1}",
                    connection_handle,
                    th.track_namespace_hash);

        // Add to state if not exist
        auto [anno_conn_it, is_new] = state_.announce_active[th.track_namespace_hash].try_emplace(connection_handle);

        if (!is_new) {
            SPDLOG_INFO("Received announce from connection handle: {0} for namespace hash: {0} is duplicate, ignoring",
                        connection_handle,
                        th.track_namespace_hash);
            return;
        }

        AnnounceResponse announce_response;
        announce_response.reason_code = quicr::Server::AnnounceResponse::ReasonCode::kOk;
        ResolveAnnounce(connection_handle, track_namespace, announce_response);

        auto& anno_tracks = state_.announce_active[th.track_namespace_hash][connection_handle];

        // Check if there are any subscribes. If so, send subscribe to announce for all tracks matching namespace
        const auto sub_active_it = state_.subscribe_active.find(th.track_namespace_hash);
        if (sub_active_it != state_.subscribe_active.end()) {
            for (const auto& [track_name, who] : sub_active_it->second) {
                if (who.size()) { // Have subscribes
                    auto& a_who = *who.begin();
                    if (anno_tracks.find(a_who.track_alias) == anno_tracks.end()) {
                        SPDLOG_INFO("Sending subscribe to announcer connection handle: {0} subscribe track_alias: {1}",
                                    connection_handle,
                                    a_who.track_alias);

                        anno_tracks.insert(a_who.track_alias); // Add track to state

                        // auto pub_track_h = state_.subscribes[a_who.track_alias][a_who.connection_handle];
                        auto sub_info_it = state_.subscribes.find(a_who.track_alias);
                        if (sub_info_it == state_.subscribes.end()) {
                            continue; // Not found
                        }

                        auto sub_info_it2 = sub_info_it->second.find(a_who.connection_handle);
                        if (sub_info_it2 == sub_info_it->second.end()) {
                            continue; // Not found
                        }

                        const auto& sub_ftn = sub_info_it2->second.track_full_name;

                        // TODO(tievens): Don't really like passing self to subscribe handler, see about fixing this
                        auto sub_track_handler = std::make_shared<LapsSubscribeTrackHandler>(sub_ftn, *this);

                        SubscribeTrack(connection_handle, sub_track_handler);
                        state_.pub_subscribes[a_who.track_alias][connection_handle] = sub_track_handler;
                    }
                }
            }
        }
    }

    void LapsServer::ConnectionStatusChanged(quicr::ConnectionHandle connection_handle, ConnectionStatus status)
    {
        if (status == ConnectionStatus::kConnected) {
            SPDLOG_DEBUG("Connection ready connection_handle: {0} ", connection_handle);
        }
    }

    quicr::Server::ClientSetupResponse LapsServer::ClientSetupReceived(
      quicr::ConnectionHandle,
      const quicr::ClientSetupAttributes& client_setup_attributes)
    {
        ClientSetupResponse client_setup_response;

        SPDLOG_INFO("Client setup received from endpoint_id: {0}", client_setup_attributes.endpoint_id);

        return client_setup_response;
    }

    void LapsServer::UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t subscribe_id)
    {
        SPDLOG_INFO("Unsubscribe connection handle: {0} subscribe_id: {1}", connection_handle, subscribe_id);

        auto ta_conn_it = state_.subscribe_alias_sub_id.find(connection_handle);
        if (ta_conn_it == state_.subscribe_alias_sub_id.end()) {
            SPDLOG_WARN("Unable to find track alias connection for connection handle: {0} subscribe_id: {1}",
                        connection_handle,
                        subscribe_id);
            return;
        }

        auto ta_it = ta_conn_it->second.find(subscribe_id);
        if (ta_it == ta_conn_it->second.end()) {
            SPDLOG_WARN("Unable to find track alias for connection handle: {0} subscribe_id: {1}",
                        connection_handle,
                        subscribe_id);
            return;
        }

        std::lock_guard<std::mutex> _(state_.state_mutex);

        auto track_alias = ta_it->second;

        ta_conn_it->second.erase(ta_it);
        if (ta_conn_it->second.empty()) {
            state_.subscribe_alias_sub_id.erase(ta_conn_it);
        }

        auto track_h_it1 = state_.subscribes.find(track_alias);
        if (track_h_it1 == state_.subscribes.end()) {
            SPDLOG_DEBUG("Unsubscribe unable to find track delegate for connection handle: {0} subscribe_id: {1}",
                         connection_handle,
                         subscribe_id);
            return;
        }

        auto track_h_it2 = track_h_it1->second.find(subscribe_id);
        if (track_h_it2 == track_h_it1->second.end()) {
            SPDLOG_DEBUG("Unsubscribe unable to find track delegate for connection handle: {0} subscribe_id: {1}",
                         connection_handle,
                         subscribe_id);
            return;
        }

        auto& track_h = track_h_it2->second.publish_handler;

        if (track_h == nullptr) {
            SPDLOG_DEBUG("Unsubscribe unable to find track delegate for connection handle: {0} subscribe_id: {1}",
                        connection_handle,
                        subscribe_id);
            return;
        }

        auto th = quicr::TrackHash(track_h->GetFullTrackName());

        state_.subscribes[track_alias].erase(connection_handle);
        bool unsub_pub{ false };
        if (state_.subscribes[track_alias].empty()) {
            unsub_pub = true;
            state_.subscribes.erase(track_alias);
        }

        state_.subscribe_active[th.track_namespace_hash][th.track_name_hash].erase(
          State::SubscribeWho{ connection_handle, subscribe_id, th.track_fullname_hash });

        if (state_.subscribe_active[th.track_namespace_hash][th.track_name_hash].empty()) {
            state_.subscribe_active[th.track_namespace_hash].erase(th.track_name_hash);
        }

        if (state_.subscribe_active[th.track_namespace_hash].empty()) {
            state_.subscribe_active.erase(th.track_namespace_hash);
        }

        if (unsub_pub) {
            SPDLOG_INFO("No subscribers left, unsubscribe publisher track_alias: {0}", track_alias);

            auto anno_ns_it = state_.announce_active.find(th.track_namespace_hash);
            if (anno_ns_it == state_.announce_active.end()) {
                return;
            }

            for (auto& [pub_connection_handle, tracks] : anno_ns_it->second) {
                if (tracks.find(th.track_fullname_hash) != tracks.end()) {
                    SPDLOG_INFO("Unsubscribe to announcer conn_id: {0} subscribe track_alias: {1}",
                                pub_connection_handle,
                                th.track_fullname_hash);

                    tracks.erase(th.track_fullname_hash); // Add track alias to state

                    auto sub_track_h = state_.pub_subscribes[th.track_fullname_hash][pub_connection_handle];
                    if (sub_track_h != nullptr) {
                        UnsubscribeTrack(pub_connection_handle, sub_track_h);
                    }
                }
            }
        }
    }

    void LapsServer::SubscribeReceived(quicr::ConnectionHandle connection_handle,
                                       uint64_t subscribe_id,
                                       [[maybe_unused]] uint64_t proposed_track_alias,
                                       const quicr::FullTrackName& track_full_name,
                                       const quicr::SubscribeAttributes&)
    {
        auto th = quicr::TrackHash(track_full_name);

        SPDLOG_INFO("New subscribe connection handle: {0} subscribe_id: {1} track alias: {2}",
                    connection_handle,
                    subscribe_id,
                    th.track_fullname_hash);

        state_.subscribe_alias_sub_id[connection_handle][subscribe_id] = th.track_fullname_hash;

        // record subscribe as active from this subscriber
        state_.subscribe_active[th.track_namespace_hash][th.track_name_hash].emplace(
          State::SubscribeWho{ connection_handle, subscribe_id, th.track_fullname_hash });
        state_.subscribe_alias_sub_id[connection_handle][subscribe_id] = th.track_fullname_hash;

        state_.subscribes[th.track_fullname_hash].emplace(
          connection_handle,
          State::SubscribePublishHandlerInfo{ track_full_name, th.track_fullname_hash, subscribe_id, nullptr });

        // Subscribe to announcer if announcer is active
        auto anno_ns_it = state_.announce_active.find(th.track_namespace_hash);
        if (anno_ns_it == state_.announce_active.end()) {
            SPDLOG_INFO("Subscribe to track namespace hash: {0}, does not have any announcements.",
                        th.track_namespace_hash);
            return;
        }

        for (auto& [conn_h, tracks] : anno_ns_it->second) {
            if (tracks.find(th.track_fullname_hash) == tracks.end()) {
                SPDLOG_INFO("Sending subscribe to announcer connection handler: {0} subscribe track_alias: {1}",
                            conn_h,
                            th.track_fullname_hash);

                tracks.insert(th.track_fullname_hash); // Add track alias to state

                // TODO(tievens): Don't really like passing self to subscriber handler, see about fixing this
                auto sub_track_h = std::make_shared<LapsSubscribeTrackHandler>(track_full_name, *this);
                SubscribeTrack(conn_h, sub_track_h);
                state_.pub_subscribes[th.track_fullname_hash][conn_h] = sub_track_h;
            }
        }
    }

    void LapsServer::MetricsSampled(const quicr::ConnectionHandle connection_handle,
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