// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include "client_manager.h"
#include "config.h"
#include "publish_handler.h"
#include "spdlog/fmt/bundled/chrono.h"
#include "subscribe_handler.h"

namespace laps {
    ClientManager::ClientManager(State& state,
                                 const Config& config,
                                 const quicr::ServerConfig& cfg,
                                 peering::PeerManager& peer_manager)
      : quicr::Server(cfg)
      , state_(state)
      , config_(config)
      , peer_manager_(peer_manager)
    {
    }

    void ClientManager::NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                              const ConnectionRemoteInfo& remote)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "New connection handle {0} accepted from {1}:{2}", connection_handle, remote.ip, remote.port);
    }

    void ClientManager::UnannounceReceived(quicr::ConnectionHandle connection_handle,
                                           const quicr::TrackNamespace& track_namespace)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_LOGGER_DEBUG(
          LOGGER,
          "Received unannounce from connection handle: {0} for namespace hash: {1}, removing all tracks "
          "associated with namespace",
          connection_handle,
          th.track_namespace_hash);

        for (auto track_alias : state_.announce_active[{ th.track_namespace_hash, connection_handle }]) {
            auto ptd = state_.pub_subscribes[{ track_alias, connection_handle }];
            if (ptd != nullptr) {
                SPDLOG_LOGGER_INFO(LOGGER,
                                   "Received unannounce from connection handle: {0} for namespace hash: {1}, removing "
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
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Purge publish state for track_alias: {0} connection handle: {1}",
                                remove_key.first,
                                remove_key.second);
        }

        std::vector<std::pair<quicr::TrackNamespaceHash, quicr::ConnectionHandle>> anno_remove_list;
        for (const auto& [key, _] : state_.announce_active) {
            if (key.second == connection_handle) {
                anno_remove_list.push_back(key);
            }
        }

        for (const auto& remove_key : anno_remove_list) {
            state_.announce_active.erase(remove_key);
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Purge publish state for namespace_hash: {0} connection handle: {1}",
                                remove_key.first,
                                remove_key.second);
        }
    }
    void ClientManager::AnnounceReceived(quicr::ConnectionHandle connection_handle,
                                         const quicr::TrackNamespace& track_namespace,
                                         const quicr::PublishAnnounceAttributes& attrs)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });
        bool notify_peering_manager{ false };

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Received announce from connection handle: {0} for namespace_hash: {} fullname_hash: {}",
                           connection_handle,
                           th.track_namespace_hash,
                           th.track_fullname_hash);

        // Add to state if not exist
        auto it = state_.announce_active.find({ th.track_namespace_hash, connection_handle });
        if (it != state_.announce_active.end()) {
            /*
             * @note Connection handle maybe reused. This requires replacing an existing entry if it's duplicate
             */
            PurgePublishState(connection_handle);
        } else {
            notify_peering_manager = true;
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
                    SPDLOG_LOGGER_INFO(
                      LOGGER,
                      "Sending subscribe to announcer connection handle: {0} subscribe track_alias: {1}",
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

        if (notify_peering_manager) {
            // Needs to be done last
            peer_manager_.ClientAnnounce({ track_namespace, {}, th.track_fullname_hash }, attrs, false);
        }
    }

    void ClientManager::ConnectionStatusChanged(quicr::ConnectionHandle connection_handle, ConnectionStatus status)
    {
        switch (status) {
            case ConnectionStatus::kConnected:
                SPDLOG_LOGGER_DEBUG(LOGGER, "Connection ready; connection_handle: {0} ", connection_handle);
                return;
            case ConnectionStatus::kConnecting:
                return;

            case ConnectionStatus::kNotConnected:
                SPDLOG_LOGGER_DEBUG(LOGGER, "Connection not connected; connection_handle: {0} ", connection_handle);
                break;
            case ConnectionStatus::kClosedByRemote:
                SPDLOG_LOGGER_DEBUG(LOGGER, "Connection closed by remote; connection_handle: {0} ", connection_handle);
                break;
            case ConnectionStatus::kIdleTimeout:
                SPDLOG_LOGGER_DEBUG(LOGGER, "Connection idle timeout; connection_handle: {0} ", connection_handle);
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

        SPDLOG_LOGGER_INFO(LOGGER, "Client setup received from endpoint_id: {0}", client_setup_attributes.endpoint_id);

        return client_setup_response;
    }

    void ClientManager::UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t subscribe_id)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "Unsubscribe connection handle: {0} subscribe_id: {1}", connection_handle, subscribe_id);

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
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Unsubscribe unable to find track handler for connection handle: {0} subscribe_id: {1}",
                                connection_handle,
                                subscribe_id);
            return;
        }

        auto ftn = sub_it->second.track_full_name;
        auto th = quicr::TrackHash(sub_it->second.track_full_name);

        if (sub_it->second.publish_handler != nullptr) {
            // TODO(tievens): Clean up publish track handler by adding unbind
        }

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
            SPDLOG_LOGGER_INFO(LOGGER, "No subscribers left, unsubscribe publisher track_alias: {0}", track_alias);
            peer_manager_.ClientUnsubscribe(ftn);
            RemovePublisherSubscribe(th);
        }
    }

    void ClientManager::RemovePublisherSubscribe(const quicr::TrackHash& track_hash)
    {
        std::vector<std::pair<quicr::messages::TrackAlias, quicr::ConnectionHandle>> remove_sub_pub;
        for (auto it = state_.pub_subscribes.lower_bound({ track_hash.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            const auto& [key, sub_to_pub_handler] = *it;

            if (key.first != track_hash.track_fullname_hash)
                break;

            SPDLOG_LOGGER_INFO(LOGGER,
                               "Unsubscribe to announcer conn_id: {0} subscribe track_alias: {1}",
                               key.second,
                               track_hash.track_fullname_hash);

            UnsubscribeTrack(key.second, sub_to_pub_handler);
            remove_sub_pub.push_back(key);
        }

        for (const auto& key : remove_sub_pub) {
            state_.pub_subscribes.erase(key);
        }
    }

    void ClientManager::SubscribeReceived(quicr::ConnectionHandle connection_handle,
                                          uint64_t subscribe_id,
                                          [[maybe_unused]] uint64_t proposed_track_alias,
                                          const quicr::FullTrackName& track_full_name,
                                          const quicr::SubscribeAttributes& attrs)
    {
        auto th = quicr::TrackHash(track_full_name);

        SPDLOG_LOGGER_INFO(LOGGER,
                           "New subscribe connection handle: {0} subscribe_id: {1} track alias: {2} priority: {3}",
                           connection_handle,
                           subscribe_id,
                           th.track_fullname_hash,
                           attrs.priority);

        ProcessSubscribe(connection_handle, subscribe_id, th, track_full_name, attrs);
    }

    void ClientManager::ProcessPeerDataObject(const peering::DataObject& data_object)
    {
        quicr::ObjectHeaders object_headers;
        object_headers.track_mode = data_object.type == peering::DataObjectType::kDatagram ? quicr::TrackMode::kDatagram
                                                                                           : quicr::TrackMode::kStream;
        object_headers.group_id = data_object.group_id;
        object_headers.subgroup_id = data_object.sub_group_id;
        object_headers.priority = data_object.priority;
        object_headers.ttl = data_object.ttl;

        quicr::messages::MoqStreamSubGroupObject object;

        // TODO: Not great to have to use stream buffer.  Clean this up later to address performance
        quicr::StreamBuffer<uint8_t> buffer;
        buffer.Push(data_object.data);
        buffer >> object;

        object_headers.extensions = object.extensions;
        object_headers.object_id = object.object_id;

        /*
        SPDLOG_LOGGER_DEBUG(LOGGER,
          "Processing peer data object tfn_hash: {} group_id: {} subgroup_id: {} object_id: {} data size: {}",
          data_object.track_full_name_hash,
          data_object.group_id,
          data_object.sub_group_id,
          object.object_id,
          object.payload.size());
          */

        // Fanout object to subscribers
        for (auto it = state_.subscribes.lower_bound({ data_object.track_full_name_hash, 0 });
             it != state_.subscribes.end();
             ++it) {
            auto& [key, sub_info] = *it;
            const auto& sub_track_alias = key.first;
            const auto& connection_handle = key.second;

            if (sub_track_alias != data_object.track_full_name_hash)
                break;

            if (sub_info.publish_handler == nullptr) {
                // Create the publish track handler and bind it on first object received
                auto pub_track_h = std::make_shared<PublishTrackHandler>(
                  sub_info.track_full_name,
                  *object_headers.track_mode,
                  sub_info.priority == 0 ? *object_headers.priority : sub_info.priority,
                  object_headers.ttl.has_value() ? *object_headers.ttl : 5000);

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                BindPublisherTrack(connection_handle, sub_info.subscribe_id, pub_track_h);
                sub_info.publish_handler = pub_track_h;
            }

            sub_info.publish_handler->PublishObject(object_headers, object.payload);
        }
    }

    void ClientManager::ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                         uint64_t subscribe_id,
                                         const quicr::TrackHash& th,
                                         const quicr::FullTrackName& track_full_name,
                                         const quicr::SubscribeAttributes& attrs)
    {
        bool is_from_peer{ false };
        if (connection_handle == 0 && subscribe_id == 0) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Processing peer subscribe track alias: {} priority: {}", th.track_fullname_hash, attrs.priority);
            is_from_peer = true;
        }

        else {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Processing subscribe connection handle: {} subscribe_id: {} track alias: {} priority: "
                                "{} ns: {} name: {}",
                                connection_handle,
                                subscribe_id,
                                th.track_fullname_hash,
                                attrs.priority,
                                th.track_namespace_hash,
                                th.track_name_hash);

            state_.subscribe_alias_sub_id[{ connection_handle, subscribe_id }] = th.track_fullname_hash;

            // record subscribe as active from this subscriber
            state_.subscribe_active_[{ th.track_namespace_hash, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle, subscribe_id, th.track_fullname_hash });
            state_.subscribe_alias_sub_id[{ connection_handle, subscribe_id }] = th.track_fullname_hash;

            const auto [_, is_new] = state_.subscribes.try_emplace(
              { th.track_fullname_hash, connection_handle },
              State::SubscribePublishHandlerInfo{
                track_full_name, th.track_fullname_hash, subscribe_id, attrs.priority, attrs.group_order, nullptr });

            if (is_new && not is_from_peer) {
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
             it != state_.announce_active.end(); ++it)
        {
            auto& [key, track_aliases] = *it;

            if (key.first != th.track_namespace_hash) {
                break;
            }

            // if we have already forwarded subscription for the track alias
            // don't forward unless we have expired the refresh period
            if(state_.pub_subscribes.count({th.track_fullname_hash, key.second}) == 0) {
                SPDLOG_LOGGER_INFO(LOGGER,
                                   "Sending subscribe to announcer connection handler: {0} subscribe track_alias: {1}",
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
            } else {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(now - last_subscription_refresh_time.value()).count();
                if (elapsed > subscription_refresh_interval_ms) {
                    SPDLOG_LOGGER_INFO(LOGGER,
                                       "Sending subscribe-update to announcer connection handler: {0} subscribe track_alias: {1}",
                                       key.second,
                                       th.track_fullname_hash);

                    auto sub_track_h = state_.pub_subscribes[{ th.track_fullname_hash, key.second }];
                    if (sub_track_h == nullptr) {
                        SPDLOG_LOGGER_INFO(LOGGER, "Subscription Handler is null");
                        return;
                    }
                    UpdateTrackSubscription(key.second, sub_track_h);
                }
            }

            last_subscription_refresh_time = std::chrono::steady_clock::now();
        }
    }

    void ClientManager::MetricsSampled(const quicr::ConnectionHandle connection_handle,
                                       const quicr::ConnectionMetrics& metrics)
    {
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Metrics connection handle: {0}"
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