// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include <quicr/defer.h>

#include "client_manager.h"
#include "config.h"
#include "fetch_handler.h"
#include "publish_handler.h"
#include "publish_namespace_handler.h"
#include "subscribe_handler.h"

#include <ranges>

namespace laps {
    ClientManager::ClientManager(State& state,
                                 const Config& config,
                                 const quicr::ServerConfig& cfg,
                                 peering::PeerManager& peer_manager,
                                 size_t cache_duration_ms)
      : quicr::Server(cfg, config.tick_service_)
      , state_(state)
      , config_(config)
      , peer_manager_(peer_manager)
      , cache_duration_ms_(cache_duration_ms)
    {
    }

    void ClientManager::NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                              const ConnectionRemoteInfo& remote)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "New connection handle {0} accepted from {1}:{2}", connection_handle, remote.ip, remote.port);
    }

    std::vector<quicr::ConnectionHandle> ClientManager::PublishNamespaceDoneReceived(
      quicr::ConnectionHandle connection_handle,
      uint64_t request_id)
    {

        auto it = state_.requests.find({ connection_handle, request_id });
        if (it == state_.requests.end()) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER,
              "Received publish namespace done from connection handle: {0} request_id: {1} but request Id not in state",
              connection_handle,
              request_id);

            return {};
        }

        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Received publish namespace done from connection handle: {0} request_id: {1}",
                            connection_handle,
                            request_id);

        auto& track_namespace = std::any_cast<quicr::TrackNamespace&>(it->second.related_data);
        auto th = quicr::TrackHash({ track_namespace, {} });

        // TODO: Fix O(prefix namespaces) matching
        std::vector<quicr::ConnectionHandle> sub_namespace_connections;
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (auto conn_handle : std::views::keys(conns)) {
                SPDLOG_DEBUG("Received publish namespace done matches prefix subscribed from connection handle: {} for "
                             "namespace hash: {}",
                             conn_handle,
                             th.track_namespace_hash);

                sub_namespace_connections.emplace_back(conn_handle);
            }
        }

        ResolvePublishNamespaceDone(connection_handle, request_id, sub_namespace_connections);

        for (auto track_alias : state_.pub_namespace_active[{ track_namespace, connection_handle }]) {
            auto ptd = state_.pub_subscribes[{ track_alias, connection_handle }];
            if (ptd != nullptr) {
                SPDLOG_LOGGER_INFO(
                  LOGGER,
                  "Received publish namespace done from connection handle: {0} for namespace hash: {1}, removing "
                  "track alias: {2}",
                  connection_handle,
                  th.track_namespace_hash,
                  track_alias);

                UnsubscribeTrack(connection_handle, ptd);
            }
            state_.pub_subscribes.erase({ track_alias, connection_handle });
        }

        state_.pub_namespace_active.erase({ track_namespace, connection_handle });

        if (connection_handle)
            peer_manager_.ClientUnannounce({ track_namespace, {} });

        return sub_namespace_connections;
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
                                "Purge publish state for track_alias: {} connection handle: {}",
                                remove_key.first,
                                remove_key.second);
        }

        std::vector<std::pair<quicr::TrackNamespace, quicr::ConnectionHandle>> anno_remove_list;
        for (const auto& [key, _] : state_.pub_namespace_active) {
            if (key.second == connection_handle) {
                anno_remove_list.push_back(key);
            }
        }

        for (const auto& remove_key : anno_remove_list) {
            state_.pub_namespace_active.erase(remove_key);
        }
    }

    void ClientManager::PublishNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                 const quicr::TrackNamespace& track_namespace,
                                                 const quicr::PublishNamespaceAttributes& attrs)
    {

        auto [req_it, _] = state_.requests.try_emplace({ connection_handle, attrs.request_id },
                                                       State::RequestTransaction::Type::kPublishNamespace,
                                                       State::RequestTransaction::State::kOk);
        req_it->second.related_data.emplace<quicr::TrackNamespace>(track_namespace);

        auto subscribe_to_publisher = [&] {
            auto& anno_tracks = state_.pub_namespace_active[{ track_namespace, connection_handle }];

            // Check if there are any subscribes. If so, send subscribe to announce for all tracks matching namespace
            for (const auto& [ns, sub_tracks] : state_.subscribe_active_) {
                if (!ns.first.HasSamePrefix(track_namespace)) {
                    continue;
                }

                if (sub_tracks.empty()) {
                    continue;
                }

                auto& a_si = *sub_tracks.begin();
                if (anno_tracks.find(a_si.track_alias) == anno_tracks.end()) {
                    SPDLOG_LOGGER_INFO(
                      LOGGER,
                      "Sending subscribe to announcer connection handle: {0} subscribe track_alias: {1}",
                      connection_handle,
                      a_si.track_alias);

                    anno_tracks.insert(a_si.track_alias); // Add track to state

                    const auto& sub_info_it = state_.subscribes.find({ a_si.track_alias, a_si.connection_handle });
                    if (sub_info_it == state_.subscribes.end()) {
                        continue;
                    }

                    const auto& sub_ftn = sub_info_it->second.track_full_name;

                    // TODO(tievens): Don't really like passing self to subscribe handler, see about fixing this
                    auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(
                      sub_ftn, 0, quicr::messages::GroupOrder::kOriginalPublisherOrder, *this, config_.tick_service_);

                    // Add subsribers to publisher subscribe handler
                    for (const auto& sub_info : sub_tracks) {
                        sub_track_handler->AddSubscriber(sub_info.connection_handle,
                                                         sub_info.request_id,
                                                         sub_info.priority,
                                                         sub_info.delivery_timeout,
                                                         sub_info.start_location);
                    }

                    SubscribeTrack(connection_handle, sub_track_handler);
                    state_.pub_subscribes[{ a_si.track_alias, connection_handle }] = sub_track_handler;
                }
            }
        };

        auto th = quicr::TrackHash({ track_namespace, {} });

        SPDLOG_LOGGER_INFO(
          LOGGER,
          "Received publish namespace from connection handle: {} for namespace_hash: {} fullname_hash: {}",
          connection_handle,
          th.track_namespace_hash,
          th.track_fullname_hash);

        // Add to state if not exist
        auto it = state_.pub_namespace_active.find({ track_namespace, connection_handle });
        if (it != state_.pub_namespace_active.end()) {
            /*
             * @note Duplicate announce from same connection handle can happen when there are
             *      more than one publish tracks (different name) but use the same namespace.
             *      In this case, we just want to send subscribes
             */
            if (connection_handle)
                subscribe_to_publisher();
            return;
        }

        state_.pub_namespace_active.try_emplace(std::make_pair(track_namespace, connection_handle));

        PublishNamespaceResponse announce_response;
        announce_response.reason_code = quicr::Server::PublishNamespaceResponse::ReasonCode::kOk;

        std::vector<quicr::ConnectionHandle> sub_annos_connections;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (auto& [conn_handle, _] : conns) {
                SPDLOG_DEBUG("Received publish namespace matches prefix subscribed namespace from connection handle: "
                             "{} for namespace hash: {}",
                             conn_handle,
                             th.track_namespace_hash);

                sub_annos_connections.emplace_back(conn_handle);
            }
        }

        ResolvePublishNamespace(
          connection_handle, attrs.request_id, track_namespace, sub_annos_connections, announce_response);

        if (connection_handle) {
            subscribe_to_publisher();

            /*
             * Always send announcements to peer manager so new clients can trigger subscribe matching and data
             * forwarding path creation. This needs to be done last to all other client work.
             */
            peer_manager_.ClientAnnounce({ track_namespace, {} }, attrs, false);
        }
    }

    void ClientManager::PublishReceived(quicr::ConnectionHandle connection_handle,
                                        uint64_t request_id,
                                        const quicr::messages::PublishAttributes& publish_attributes,
                                        [[maybe_unused]] std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler)
    {
        bool is_from_peer = !connection_handle && !request_id;
        auto th = quicr::TrackHash(publish_attributes.track_full_name);

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Received publish from connection handle: {} using track alias: {} request_id: {}",
                           connection_handle,
                           th.track_fullname_hash,
                           request_id);

        quicr::PublishResponse publish_response;
        publish_response.reason_code = quicr::PublishResponse::ReasonCode::kOk;

        // PublishTrack within publish namespace handler if matched
        for (auto& [tn, conns] : state_.subscribes_namespaces) {
            const auto prefix_match = tn.IsPrefixOf(publish_attributes.track_full_name.name_space);

            if (prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) {
                for (auto& [_, pub_ns_h] : conns) {
                    if (pub_ns_h->GetConnectionId() == connection_handle) {
                        // Initially do not mirror
                        continue;
                    }

                    const auto handler = PublishTrackHandler::Create(publish_attributes.track_full_name,
                                                                     quicr::TrackMode::kStream,
                                                                     publish_attributes.priority,
                                                                     publish_attributes.delivery_timeout.count(),
                                                                     publish_attributes.start_location,
                                                                     *this);
                    if (!handler->GetTrackAlias().has_value()) {
                        handler->SetTrackAlias(th.track_fullname_hash);
                    }

                    pub_ns_h->PublishTrack(handler);
                }
            }
        }

        // passively create the subscribe handler towards the publisher
        auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(publish_attributes.track_full_name,
                                                                         0,
                                                                         quicr::messages::GroupOrder::kAscending,
                                                                         *this,
                                                                         config_.tick_service_,
                                                                         true);

        if (is_from_peer) {
            // For peering, need to set the track alias
            sub_track_handler->SetTrackAlias(publish_attributes.track_alias);
            sub_track_handler->SetFromPeer();
        }

        if (publish_attributes.dynamic_groups) {
            sub_track_handler->SupportNewGroupRequest(true);
        }

        state_.pub_subscribes[{ th.track_fullname_hash, connection_handle }] = sub_track_handler;

        if (!is_from_peer) {
            state_.pub_subscribes_by_req_id[{ request_id, connection_handle }] = sub_track_handler;

            // Do this before pause to maintain MOQT message sequence order
            ResolvePublish(connection_handle, request_id, publish_attributes, publish_response, sub_track_handler);

            /*
             * Always send publish as an announcement to peer manager so new clients can trigger subscribe matching and
             * data forwarding path creation. This needs to be done last to all other client work.
             */
            peer_manager_.ClientAnnounce(publish_attributes.track_full_name, {}, false);
        }

        // Check if there are any subscribers
        bool has_subs{ peer_manager_.HasSubscribers(th.track_fullname_hash) };
        for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 }); it != state_.subscribes.end();
             ++it) {
            if (it->first.first == th.track_fullname_hash) {
                has_subs = true;
                sub_track_handler->AddSubscriber(it->first.second,
                                                 it->second.request_id,
                                                 it->second.priority,
                                                 std::chrono::milliseconds(it->second.object_ttl),
                                                 it->second.start_location);
            }
        }

        quicr::messages::TrackNamespace sub_ns;
        for (const auto& [ns_prefix, conns] : state_.subscribes_namespaces) {
            if (ns_prefix.HasSamePrefix(publish_attributes.track_full_name.name_space) && !conns.empty()) {
                has_subs = true;

                if (sub_ns.empty()) {
                    sub_ns = ns_prefix;
                }

                for (const auto& [conn_id, ns_handler] : conns) {
                    sub_track_handler->AddSubscribeNamespace(ns_handler);
                }
            }
        }

        auto ns_th = quicr::TrackHash({ sub_ns, {} });
        auto rank_it = track_rankings_.find(ns_th.track_namespace_hash);
        if (rank_it != track_rankings_.end()) {
            sub_track_handler->SetTrackRanking(rank_it->second);
        }

        if (!has_subs) {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "No subscribers, pause publish connection handle: {0} using track alias: {1}",
                               connection_handle,
                               th.track_fullname_hash);

            sub_track_handler->Pause();
        }
    }

    void ClientManager::SubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                   quicr::DataContextId data_ctx_id,
                                                   const quicr::TrackNamespace& prefix_namespace,
                                                   const quicr::messages::SubscribeNamespaceAttributes& attributes)
    {
        auto th = quicr::TrackHash({ prefix_namespace, {} });

        auto [it, is_new] = state_.subscribes_namespaces.try_emplace(prefix_namespace);

        auto handler = PublishNamespaceHandler::Create(prefix_namespace, GetTickService());
        PublishNamespace(connection_handle, handler);

        auto [pub_it, _] = it->second.emplace(connection_handle, handler);

        if (is_new) {
            SPDLOG_INFO("Subscribe namespace received connection handle: {} for namespace_hash: {}, adding to state",
                        connection_handle,
                        th.track_namespace_hash);
        }

        auto [ranks_it, __] = track_rankings_.try_emplace(th.track_namespace_hash, std::make_shared<TrackRanking>());
        ranks_it->second->AddNamespaceHandler(handler);

        std::vector<quicr::TrackNamespace> matched_ns;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [key, _] : state_.pub_namespace_active) {
            // Add matching announced namespaces to vector without duplicates
            const auto prefix_match = prefix_namespace.IsPrefixOf(key.first);
            if ((prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) &&
                (matched_ns.empty() || matched_ns.back() != key.first)) {
                matched_ns.push_back(key.first);
            }
        }

        // TODO: Need to change this to use what peering is using to prefix match instead of O(n) over all publish
        //  subscribes
        for (const auto& [ta_conn, handler] : state_.pub_subscribes) {
            if (ta_conn.second == connection_handle || !handler) {
                continue;
            }

            const auto& track_full_name = handler->GetFullTrackName();
            const auto prefix_match = prefix_namespace.IsPrefixOf(track_full_name.name_space);
            if (prefix_match == std::partial_ordering::greater || prefix_match == std::partial_ordering::equivalent) {
                std::optional<quicr::messages::Location> largest_location = GetLargestAvailable(track_full_name);

                /*
                 * PublishTrack within the namespace handler determines if the track should be published or not
                 * based on filters
                 */
                const auto pub_handler = PublishTrackHandler::Create(
                  track_full_name,
                  quicr::TrackMode::kStream,
                  handler->GetPriority(),
                  handler->GetDeliveryTimeout().value_or(std::chrono::milliseconds(kDefaultObjectTtl)).count(),
                  largest_location.value_or(quicr::messages::Location{ 0, 0 }),
                  *this);

                if (!pub_handler->GetTrackAlias().has_value()) {
                    auto pub_th = quicr::TrackHash(track_full_name);
                    pub_handler->SetTrackAlias(pub_th.track_fullname_hash);
                }
                pub_it->second->PublishTrack(pub_handler);

                handler->AddSubscribeNamespace(pub_it->second);
                handler->SetTrackRanking(ranks_it->second);

                SPDLOG_LOGGER_DEBUG(
                  LOGGER,
                  "Matched PUBLISH track for SUBSCRIBE_NAMESPACE: conn: {} track_alias: {} track_hash: {}",
                  connection_handle,
                  ta_conn.first,
                  quicr::TrackHash(track_full_name).track_fullname_hash);
            }
        }

        const quicr::SubscribeNamespaceResponse response = { .reason_code =
                                                               quicr::SubscribeNamespaceResponse::ReasonCode::kOk,
                                                             .namespaces = std::move(matched_ns) };
        ResolveSubscribeNamespace(connection_handle, data_ctx_id, attributes.request_id, prefix_namespace, response);
    }

    void ClientManager::UnsubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                     const quicr::TrackNamespace& prefix_namespace)
    {
        auto it = state_.subscribes_namespaces.find(prefix_namespace);
        if (it == state_.subscribes_namespaces.end()) {
            return;
        }

        auto th = quicr::TrackHash({ prefix_namespace, {} });
        SPDLOG_INFO("Unsubscribe namspace received connection handle: {} for namespace_hash: {}, removing",
                    connection_handle,
                    th.track_namespace_hash);

        // Get the publish namespace handler by connection handle
        auto pub_it = it->second.find(connection_handle);
        if (pub_it == it->second.end()) {
            if (it->second.empty()) {
                state_.subscribes_namespaces.erase(it);
            }
            return;
        }

        // Loop through publishes and remove subscribe namespace
        for (const auto& [ta_conn, handler] : state_.pub_subscribes) {
            if (ta_conn.second == connection_handle || !handler) {
                continue;
            }

            const auto& track_full_name = handler->GetFullTrackName();
            const bool ns_matched = prefix_namespace.HasSamePrefix(track_full_name.name_space);
            if (ns_matched) {
                handler->RemoveSubscribeNamespace(pub_it->second);
                track_rankings_[th.track_namespace_hash]->RemoveNamespaceHandler(pub_it->second);

                RemoveOrPausePublisherSubscribe(ta_conn.first);
            }
        }

        it->second.erase(pub_it);

        if (it->second.empty()) {
            state_.subscribes_namespaces.erase(it);
            track_rankings_.erase(th.track_namespace_hash);
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

        // Remove all subscribe announces for this connection handle
        std::vector<quicr::TrackNamespace> remove_ns;
        for (auto& [ns, conns] : state_.subscribes_namespaces) {
            auto it = conns.find(connection_handle);
            if (it != conns.end()) {
                remove_ns.emplace_back(ns);
            }
        }

        for (auto ns : remove_ns) {
            UnsubscribeNamespaceReceived(connection_handle, ns);
        }

        // Clean up subscribe states
        std::vector<std::pair<quicr::ConnectionHandle, quicr::messages::RequestID>> unsub_list;
        for (auto it = state_.subscribe_alias_req_id.lower_bound({ connection_handle, 0 });
             it != state_.subscribe_alias_req_id.end();
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

    void ClientManager::PublishDoneReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "Publish Done connection handle: {0} request_id: {1}", connection_handle, request_id);

        const auto s_it = state_.pub_subscribes_by_req_id.find({ request_id, connection_handle });
        if (s_it == state_.pub_subscribes_by_req_id.end()) {
            SPDLOG_LOGGER_WARN(LOGGER,
                               "Unable to find subscribe by request id for connection handle: {0} request_id: {1}",
                               connection_handle,
                               request_id);
            return;
        }

        if (connection_handle)
            peer_manager_.ClientAnnounce(s_it->second->GetFullTrackName(), {}, true);

        std::unique_lock<std::mutex> lock(state_.state_mutex);

        auto th = quicr::TrackHash(s_it->second->GetFullTrackName());

        state_.pub_subscribes.erase({ th.track_fullname_hash, connection_handle });

        bool have_publishers{ false };
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {

            if (it->first.first != th.track_fullname_hash) {
                break;
            }

            have_publishers = true;
            break;
        }

        std::vector<std::pair<quicr::ConnectionHandle, quicr::messages::RequestID>> unsub_list;

        if (!have_publishers) {
            // Find subscribers that match this publisher and unsubscribe
            for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 }); it != state_.subscribes.end();
                 ++it) {
                if (it->first.first != th.track_fullname_hash) {
                    break;
                }

                SPDLOG_LOGGER_INFO(LOGGER,
                                   "No publishers left, unsubscribe conn_id: {} track_alias: {} request_id: {}",
                                   it->first.second,
                                   it->second.track_alias,
                                   it->second.request_id);

                unsub_list.emplace_back(it->first.second, it->second.request_id);
            }
        }

        // TODO: check if subscribe requests detatch or not

        if (!config_.detached_subs) {
            lock.unlock();
            for (auto& [c_handle, req_id] : unsub_list) {
                UnsubscribeReceived(c_handle, req_id);
            }

            lock.lock();
        }

        state_.pub_subscribes_by_req_id.erase(s_it);
    }

    void ClientManager::UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id)
    {
        SPDLOG_LOGGER_INFO(LOGGER, "Unsubscribe connection handle: {0} request_id: {1}", connection_handle, request_id);

        const auto ta_it = state_.subscribe_alias_req_id.find({ connection_handle, request_id });
        if (ta_it == state_.subscribe_alias_req_id.end()) {
            SPDLOG_WARN(
              "Unable to find track alias for connection handle: {0} request_id: {1}", connection_handle, request_id);
            return;
        }

        std::lock_guard<std::mutex> _(state_.state_mutex);

        auto track_alias = ta_it->second;

        const auto sub_it = state_.subscribes.find({ track_alias, connection_handle });
        if (sub_it == state_.subscribes.end()) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Unsubscribe unable to find track handler for connection handle: {0} request_id: {1}",
                                connection_handle,
                                request_id);
            return;
        }

        state_.subscribe_alias_req_id.erase(ta_it);

        auto ftn = sub_it->second.track_full_name;
        auto th = quicr::TrackHash(sub_it->second.track_full_name);

        // Remove subscribe from publisher subscribe handler
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            const auto& [key, sub_to_pub_handler] = *it;
            const auto& [track_alias, pub_conn_handle] = key;

            if (track_alias != th.track_fullname_hash)
                break;

            sub_to_pub_handler->RemoveSubscriber(connection_handle);
        }

        auto& sub_active_list =
          state_.subscribe_active_[{ sub_it->second.track_full_name.name_space, th.track_name_hash }];
        sub_active_list.erase(State::SubscribeInfo{ connection_handle, request_id, th.track_fullname_hash });

        if (sub_active_list.empty()) {
            state_.subscribe_active_.erase({ sub_it->second.track_full_name.name_space, th.track_name_hash });
        }

        state_.subscribes.erase(sub_it);

        RemoveOrPausePublisherSubscribe(th.track_fullname_hash);
    }

    void ClientManager::RemoveOrPausePublisherSubscribe(quicr::TrackFullNameHash track_fullname_hash)
    {
        // Do nothing if peering still has a subscriber
        if (peer_manager_.HasSubscribers(track_fullname_hash)) {
            return;
        }

        bool has_subs{ false };

        std::vector<std::pair<quicr::messages::TrackAlias, quicr::ConnectionHandle>> remove_sub_pub;
        for (auto it = state_.pub_subscribes.lower_bound({ track_fullname_hash, 0 }); it != state_.pub_subscribes.end();
             ++it) {
            const auto& [key, sub_to_pub_handler] = *it;

            if (key.first != track_fullname_hash)
                break;

            if (sub_to_pub_handler->HasSubscribers()) {
                has_subs = true;
                // Do not pause or remove handler if there are still some subscriber/subscribe namespaces
                has_subs = true;
                continue;
            }

            if (sub_to_pub_handler->IsPublisherInitiated()) {
                SPDLOG_LOGGER_INFO(LOGGER,
                                   "No subscribers left, pause publisher conn_id: {0} track_alias: {1}",
                                   key.second,
                                   track_fullname_hash);

                sub_to_pub_handler->Pause();
            } else {
                SPDLOG_LOGGER_INFO(LOGGER,
                                   "No subscribers left, unsubscribe publisher conn_id: {0} track_alias: {1}",
                                   key.second,
                                   track_fullname_hash);

                UnsubscribeTrack(key.second, sub_to_pub_handler);
                remove_sub_pub.push_back(key);
            }
        }

        for (const auto& key : remove_sub_pub) {
            state_.pub_subscribes.erase(key);
        }

        if (!has_subs) {
            peer_manager_.ClientUnsubscribe(track_fullname_hash);
        }
    }

    void ClientManager::TrackStatusReceived(quicr::ConnectionHandle connection_handle,
                                            uint64_t request_id,
                                            const quicr::FullTrackName& track_full_name)
    {
        auto th = quicr::TrackHash(track_full_name);

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Track status request connection handle: {} request_id: {} track alias: {}",
                           connection_handle,
                           request_id,
                           th.track_fullname_hash);

        const auto largest = GetLargestAvailable(track_full_name);

        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {

            if (it->first.first != th.track_fullname_hash) {
                break;
            }

            if (it->first.second != connection_handle) {
                ResolveTrackStatus(connection_handle,
                                   request_id,
                                   {
                                     quicr::RequestResponse::ReasonCode::kOk,
                                     it->second->IsPublisherInitiated(),
                                     std::nullopt,
                                     largest,
                                   });
                return;
            }
        }

        ResolveTrackStatus(connection_handle,
                           request_id,
                           {
                             quicr::RequestResponse::ReasonCode::kDoesNotExist,
                             false,
                             "Track does not exist",
                             std::nullopt,
                           });
    }

    void ClientManager::SubscribeReceived(quicr::ConnectionHandle connection_handle,
                                          uint64_t request_id,
                                          const quicr::FullTrackName& track_full_name,
                                          const quicr::messages::SubscribeAttributes& attrs)
    {
        auto th = quicr::TrackHash(track_full_name);

        auto it = state_.subscribes.find({ th.track_fullname_hash, connection_handle });
        if (it != state_.subscribes.end()) {
            ResolveSubscribe(connection_handle,
                             request_id,
                             th.track_fullname_hash,
                             { .reason_code = quicr::RequestResponse::ReasonCode::kNotSupported,
                               .is_publisher_initiated = false,
                               .error_reason = "Duplicate subscribe" });

            SPDLOG_LOGGER_INFO(
              LOGGER,
              "Duplicate subscribe connection handle: {0} request_id: {1} track alias: {2} priority: {3}",
              connection_handle,
              request_id,
              th.track_fullname_hash,
              attrs.priority);

            return;
        }

        SPDLOG_LOGGER_INFO(
          LOGGER,
          "New subscribe connection handle: {} request_id: {} track alias: {} priority: {} delivery timeout: {}ms",
          connection_handle,
          request_id,
          th.track_fullname_hash,
          attrs.priority,
          attrs.delivery_timeout.count());

        auto largest = GetLargestAvailable(track_full_name);
        if (largest.has_value()) {
            ResolveSubscribe(
              connection_handle,
              request_id,
              th.track_fullname_hash,
              { quicr::RequestResponse::ReasonCode::kOk, attrs.is_publisher_initiated, std::nullopt, largest });
        } else {
            ResolveSubscribe(
              connection_handle, request_id, th.track_fullname_hash, { quicr::RequestResponse::ReasonCode::kOk });
        }

        ProcessSubscribe(connection_handle, request_id, th, track_full_name, attrs, largest);
    }

    std::optional<quicr::messages::Location> ClientManager::GetLargestAvailable(const quicr::FullTrackName& track_name)
    {
        // Get the largest object from the cache.
        std::optional<uint64_t> largest_group_id = std::nullopt;
        std::optional<uint64_t> largest_object_id = std::nullopt;

        const auto& th = quicr::TrackHash(track_name);
        const auto cache_entry_it = cache_.find(th.track_fullname_hash);
        if (cache_entry_it != cache_.end()) {
            auto& [_, cache] = *cache_entry_it;
            if (const auto& latest_group = cache.Last(); latest_group && !latest_group->empty()) {
                const auto& latest_object = *std::prev(latest_group->end());
                largest_group_id = latest_object.headers.group_id;
                largest_object_id = latest_object.headers.object_id;
            }
        }
        if (!largest_group_id.has_value() || !largest_object_id.has_value()) {
            return std::nullopt;
        }
        return quicr::messages::Location{ .group = largest_group_id.value(), .object = largest_object_id.value() };
    }

    void ClientManager::FetchReceived(quicr::ConnectionHandle connection_handle,
                                      uint64_t request_id,
                                      const quicr::FullTrackName& track_full_name,
                                      uint8_t priority,
                                      quicr::messages::GroupOrder group_order,
                                      quicr::messages::Location start,
                                      quicr::messages::FetchEndLocation end)
    {
        auto reason_code = quicr::FetchResponse::ReasonCode::kOk;
        std::optional<quicr::messages::Location> largest_location = std::nullopt;
        auto th = quicr::TrackHash(track_full_name);

        auto cache_entry_it = cache_.find(th.track_fullname_hash);
        if (cache_entry_it != cache_.end()) {
            auto& [_, cache] = *cache_entry_it;
            if (const auto& latest_group = cache.Last(); latest_group && !latest_group->empty()) {
                const auto& latest_object = *std::prev(latest_group->end());
                largest_location = { latest_object.headers.group_id, latest_object.headers.object_id };
            }
        }

        if (!largest_location.has_value()) {
            // TODO: This changes to send an empty object instead of REQUEST_ERROR
            reason_code = quicr::FetchResponse::ReasonCode::kNoObjects;
        } else if (start.group > end.group || largest_location->group < start.group) {
            reason_code = quicr::FetchResponse::ReasonCode::kInvalidRange;
        }

        const auto& cache_entries = cache_entry_it != cache_.end()
                                      ? cache_entry_it->second.Get(start.group, end.group)
                                      : std::vector<std::shared_ptr<std::set<CacheObject>>>{};

        if (cache_entries.empty() && reason_code == quicr::FetchResponse::ReasonCode::kOk) {
            reason_code = quicr::FetchResponse::ReasonCode::kNoObjects;
        }

        // TODO: Adjust the TTL to allow more time for transmission
        auto pub_fetch_h =
          quicr::PublishFetchHandler::Create(track_full_name, priority, request_id, group_order, config_.object_ttl_);
        BindFetchTrack(connection_handle, pub_fetch_h);

        stop_fetch_.try_emplace({ connection_handle, request_id }, false);

        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Fetch received conn_id: {} request_id: {} range start group: {} start object: {} end "
                            "group: {} end object: {} largest_location: {}",
                            connection_handle,
                            request_id,
                            start.group,
                            start.object,
                            end.group,
                            end.object.value_or(0),
                            largest_location.has_value() ? largest_location.value().group : 0);

        std::thread retrieve_cache_thread([=, cache_entries = std::move(cache_entries), this] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

            auto rc = reason_code;

            if (rc != quicr::FetchResponse::ReasonCode::kOk) {
                // Try to see if original publisher can provide the data
                auto track_handler = FetchTrackHandler::Create(pub_fetch_h,
                                                               track_full_name,
                                                               priority,
                                                               group_order,
                                                               { .group = start.group, .object = start.object },
                                                               { .group = end.group, .object = end.object });

                quicr::ConnectionHandle pub_connection_handle = 0;

                // Find the publisher connection handle to send the fetch request
                // TODO: Add peering support
                {
                    std::lock_guard _(state_.state_mutex);
                    for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
                         it != state_.pub_subscribes.end();
                         ++it) {
                        auto& track_alias = it->first.first;
                        auto& pub_conn_id = it->first.second;

                        if (track_alias != th.track_fullname_hash) {
                            rc = quicr::FetchResponse::ReasonCode::kNoObjects;
                            break;
                        }

                        pub_connection_handle = pub_conn_id;
                        break; // TODO: Support multiple publishers
                    }
                }

                if (pub_connection_handle) {
                    SPDLOG_LOGGER_DEBUG(LOGGER,
                                        "Fetch received conn_id: {} request_id: {}, sending to publisher conn_id: {}",
                                        connection_handle,
                                        request_id,
                                        pub_connection_handle);
                    FetchTrack(pub_connection_handle, track_handler);

                    for (int to = 0; to < kFetchUpstreamMaxWaitMs; to += 5) {
                        if (track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kPendingResponse &&
                            track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kNotSubscribed) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }

                    switch (track_handler->GetStatus()) {
                        case quicr::FetchTrackHandler::Status::kDoneByFin:
                            [[fallthrough]];
                        case quicr::FetchTrackHandler::Status::kOk:
                            rc = quicr::FetchResponse::ReasonCode::kOk;
                            break;
                        case quicr::FetchTrackHandler::Status::kError:
                            rc = quicr::FetchResponse::ReasonCode::kNoObjects;
                            break;
                        default:
                            rc = quicr::FetchResponse::ReasonCode::kInternalError;
                            break;
                    }
                }

                ResolveFetch(connection_handle,
                             request_id,
                             priority,
                             group_order,
                             {
                               rc,
                               rc == quicr::FetchResponse::ReasonCode::kOk ? std::nullopt
                                                                           : std::make_optional("Cannot process fetch"),
                               track_handler->GetLatestLocation(),
                             });

                while (track_handler->GetStatus() == quicr::FetchTrackHandler::Status::kOk) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                CancelFetchTrack(pub_connection_handle, track_handler);
                return;
            }

            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Fetch received conn_id: {} request_id: {}, using cache", connection_handle, request_id);
            ResolveFetch(
              connection_handle,
              request_id,
              priority,
              group_order,
              {
                rc,
                rc == quicr::FetchResponse::ReasonCode::kOk ? std::nullopt : std::make_optional("Cannot process fetch"),
                largest_location,
              });

            for (const auto& entry : cache_entries) {
                for (const auto& object : *entry) {
                    if (stop_fetch_[{ connection_handle, request_id }]) {
                        stop_fetch_.erase({ connection_handle, request_id });
                        return;
                    }

                    // Start at start object id
                    if (start.group == object.headers.group_id && start.object > object.headers.object_id) {
                        continue;
                    }

                    // Stop at end object, unless end object is zero
                    if (end.object.has_value() && object.headers.group_id == end.group &&
                        object.headers.object_id > *end.object) {
                        return;
                    }

                    SPDLOG_LOGGER_TRACE(
                      LOGGER, "Fetching group: {} object: {}", object.headers.group_id, object.headers.object_id);

                    try {
                        pub_fetch_h->PublishObject(object.headers, object.data);
                    } catch (const std::exception& e) {
                        SPDLOG_LOGGER_ERROR(LOGGER, "Caught exception sending fetch object: {}", e.what());
                    }
                }
            }
        });

        retrieve_cache_thread.detach();
    }

    void ClientManager::StandaloneFetchReceived(quicr::ConnectionHandle connection_handle,
                                                uint64_t request_id,
                                                const quicr::FullTrackName& track_full_name,
                                                const quicr::messages::StandaloneFetchAttributes& attributes)
    {
        FetchReceived(connection_handle,
                      request_id,
                      track_full_name,
                      attributes.priority,
                      attributes.group_order,
                      attributes.start_location,
                      attributes.end_location);
    }

    void ClientManager::JoiningFetchReceived(quicr::ConnectionHandle connection_handle,
                                             uint64_t request_id,
                                             const quicr::FullTrackName& track_full_name,
                                             const quicr::messages::JoiningFetchAttributes& attributes)
    {
        std::optional<quicr::messages::Location> largest_location = GetLargestAvailable(track_full_name);

        // No largest location is an error.
        if (!largest_location.has_value()) {
            ResolveFetch(connection_handle,
                         request_id,
                         attributes.priority,
                         attributes.group_order,
                         {
                           quicr::FetchResponse::ReasonCode::kInvalidRange,
                           "No objects available for joining fetch",
                           std::nullopt,
                         });
            return;
        }

        uint64_t joining_start = 0;
        if (attributes.relative) {
            if (largest_location->group > attributes.joining_start)
                joining_start = largest_location->group - attributes.joining_start;
        } else {
            joining_start = attributes.joining_start;
        }

        FetchReceived(connection_handle,
                      request_id,
                      track_full_name,
                      attributes.priority,
                      attributes.group_order,
                      { joining_start, 0 },
                      { largest_location->group, std::nullopt });
    }

    void ClientManager::FetchCancelReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id)
    {
        SPDLOG_INFO("Canceling fetch for connection_handle: {} request_id: {}", connection_handle, request_id);

        if (stop_fetch_.count({ connection_handle, request_id }) == 0)
            stop_fetch_[{ connection_handle, request_id }] = true;
    }

    void ClientManager::NewGroupRequested(const quicr::FullTrackName& track_full_name,
                                          quicr::messages::GroupId group_id)
    {
        auto th = quicr::TrackHash(track_full_name);
        SPDLOG_INFO("New group requested received track_alais: {} group_id: {} ", th.track_fullname_hash, group_id);

        // Update peering subscribe info - This will update existing instead of creating new
        peer_manager_.ClientSubscribeUpdate(track_full_name,
                                            {
                                              kDefaultPriority,
                                              quicr::messages::GroupOrder::kAscending,
                                              std::chrono::milliseconds(kDefaultObjectTtl),
                                              std::chrono::milliseconds(0),
                                              std::monostate{},
                                              1,
                                              true,
                                            });

        // Notify all publishers that there is a new group request
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            auto& track_alias = it->first.first;
            auto& pub_conn_id = it->first.second;

            if (track_alias != th.track_fullname_hash) {
                break;
            }

            if (!it->second->GetPendingNewRquestId().has_value() ||
                (group_id == 0 && *it->second->GetPendingNewRquestId()) ||
                *it->second->GetPendingNewRquestId() < group_id) {

                it->second->SetNewGroupRequestId(group_id);
                DampenOrUpdateTrackSubscription(it->second, true);
            }
        }
    }

    bool ClientManager::DampenOrUpdateTrackSubscription(std::shared_ptr<SubscribeTrackHandler> sub_to_pub_track_handler,
                                                        bool new_group_request)
    {
        if (sub_to_pub_track_handler->GetConnectionId() <= 1) {
            // No updates sent to peering
            return false;
        }

        auto now = std::chrono::steady_clock::now();

        uint64_t elapsed = config_.sub_dampen_ms_ + 1;

        if (sub_to_pub_track_handler->pub_last_update_info_.time.has_value()) {
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - *sub_to_pub_track_handler->pub_last_update_info_.time)
                        .count();
        }

        if (new_group_request || elapsed > config_.sub_dampen_ms_) {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "Sending subscribe-update to publisher connection handler: {} subscribe "
                               "track_alias: {} new_group: {} pending_new_group_id: {}",
                               sub_to_pub_track_handler->GetConnectionId(),
                               sub_to_pub_track_handler->GetTrackAlias().value(),
                               new_group_request,
                               sub_to_pub_track_handler->GetPendingNewRquestId().has_value()
                                 ? *sub_to_pub_track_handler->GetPendingNewRquestId()
                                 : 0);

            sub_to_pub_track_handler->pub_last_update_info_.time = now;

            if (new_group_request) {
                sub_to_pub_track_handler->RequestNewGroup();
            } else {
                UpdateTrackSubscription(sub_to_pub_track_handler->GetConnectionId(), sub_to_pub_track_handler);
            }
        }

        return false;
    }

    void ClientManager::ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                         uint64_t request_id,
                                         const quicr::TrackHash& th,
                                         const quicr::FullTrackName& track_full_name,
                                         const quicr::messages::SubscribeAttributes& attrs,
                                         std::optional<quicr::messages::Location> largest)
    {

        auto start_location = attrs.start_location;

        if (largest.has_value()) {
            SPDLOG_LOGGER_INFO(LOGGER, "Subscribe largest group: {} object: {}", largest->group, largest->object);
        }

        if (connection_handle == 0 && request_id == 0) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Processing peer subscribe track alias: {} priority: {} new_group_request: {}",
                                th.track_fullname_hash,
                                attrs.priority,
                                attrs.new_group_request_id.has_value() ? *attrs.new_group_request_id : -1);
        }

        else {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "Processing subscribe connection handle: {} request_id: {} track alias: {} priority: "
                               "{} ns: {} name: {} new_group_request: {} start group: {} object: {}",
                               connection_handle,
                               request_id,
                               th.track_fullname_hash,
                               attrs.priority,
                               th.track_namespace_hash,
                               th.track_name_hash,
                               attrs.new_group_request_id.has_value() ? *attrs.new_group_request_id : -1,
                               start_location.group,
                               start_location.object);

            // record subscribe as active from this subscriber
            state_.subscribe_active_[{ track_full_name.name_space, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle,
                                    request_id,
                                    th.track_fullname_hash,
                                    attrs.priority,
                                    attrs.delivery_timeout,
                                    attrs.start_location });
            state_.subscribe_alias_req_id[{ connection_handle, request_id }] = th.track_fullname_hash;

            auto [sub_it, _] = state_.subscribes.try_emplace(
              { th.track_fullname_hash, connection_handle },
              State::SubscribePublishHandlerInfo{ track_full_name,
                                                  th.track_fullname_hash,
                                                  request_id,
                                                  attrs.priority,
                                                  static_cast<uint32_t>(attrs.delivery_timeout.count()),
                                                  attrs.group_order,
                                                  {} });

            // Always send updates to peers to support subscribe updates and refresh group support
            auto params = quicr::messages::Parameters{}
                            .Add(quicr::messages::ParameterType::kSubscriberPriority, attrs.priority)
                            .Add(quicr::messages::ParameterType::kGroupOrder, attrs.group_order);

            quicr::messages::Subscribe sub(request_id, track_full_name.name_space, track_full_name.name, params);

            // TODO: Current new group is not sent by client in subscribe. It's only in subscribe updates.

            quicr::Bytes sub_data;
            sub_data << sub;

            auto mt_sz = quicr::UintVar::Size(*quicr::UintVar(sub_data).begin());
            sub_data.erase(sub_data.begin(), sub_data.begin() + mt_sz + sizeof(uint16_t));

            peer_manager_.ClientSubscribe(track_full_name, attrs, sub_data);
        }

        // Resume publisher initiated subscribes
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            if (it->first.first != th.track_fullname_hash) {
                break;
            }
            if (it->second->IsPublisherInitiated()) {
                it->second->Resume();
            }

            it->second->AddSubscriber(
              connection_handle, request_id, attrs.priority, attrs.delivery_timeout, attrs.start_location);

            DampenOrUpdateTrackSubscription(it->second, attrs.new_group_request_id.has_value());
        }

        // Subscribe to announcer if announcer is active
        for (auto& [key, track_aliases] : state_.pub_namespace_active) {
            if (!key.first.HasSamePrefix(track_full_name.name_space) || !key.second) {
                continue;
            }

            // if we have already forwarded subscription for the track alias
            // don't forward unless we have expired the refresh period
            auto pub_handler_it = state_.pub_subscribes.find({ th.track_fullname_hash, key.second });
            if (pub_handler_it == state_.pub_subscribes.end()) {
                SPDLOG_LOGGER_INFO(LOGGER,
                                   "Sending subscribe to announcer connection handler: {0} subscribe track_alias: {1}",
                                   key.second,
                                   th.track_fullname_hash);

                track_aliases.insert(th.track_fullname_hash); // Add track alias to state
                auto sub_track_h =
                  std::make_shared<SubscribeTrackHandler>(track_full_name,
                                                          0 /* use zero to indicate to use publisher priority */,
                                                          quicr::messages::GroupOrder::kAscending,
                                                          *this,
                                                          config_.tick_service_);
                SubscribeTrack(key.second, sub_track_h);

                sub_track_h->AddSubscriber(
                  connection_handle, request_id, attrs.priority, attrs.delivery_timeout, attrs.start_location);

                state_.pub_subscribes_by_req_id[{ sub_track_h->GetRequestId().value(), key.second }] = sub_track_h;
                state_.pub_subscribes[{ th.track_fullname_hash, key.second }] = sub_track_h;

                if (attrs.new_group_request_id) {
                    sub_track_h->RequestNewGroup();
                }

            } else {
                auto pub_handler_it = state_.pub_subscribes.find({ th.track_fullname_hash, key.second });
                if (pub_handler_it != state_.pub_subscribes.end()) {
                    DampenOrUpdateTrackSubscription(pub_handler_it->second, attrs.new_group_request_id.has_value());
                }
            }
        }
    }

    void ClientManager::PeerUnsubscribeTrack(quicr::TrackFullNameHash track_full_name_hash)
    {
        for (auto it = state_.pub_subscribes.lower_bound({ track_full_name_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            if (it->first.first != track_full_name_hash) {
                break;
            }

            it->second->RemoveSubscriber(0);
        }
    }

    void ClientManager::PeerDataReceived(quicr::TrackFullNameHash track_full_name_hash,
                                         bool is_new_stream,
                                         std::optional<uint64_t> stream_id,
                                         std::shared_ptr<const std::vector<uint8_t>> data)
    {
        const auto it = state_.pub_subscribes.find({ track_full_name_hash, 0 });
        if (it == state_.pub_subscribes.end()) {
            return;
        }

        if (stream_id.has_value()) {
            it->second->StreamDataRecv(is_new_stream, *stream_id, data);
        } else {
            it->second->DgramDataRecv(data);
        }
    }

    void ClientManager::PeerStreamClosed(quicr::TrackFullNameHash track_full_name_hash, uint64_t stream_id, bool reset)
    {
        const auto it = state_.pub_subscribes.find({ track_full_name_hash, 0 });
        if (it == state_.pub_subscribes.end()) {
            return;
        }

        it->second->StreamClosed(stream_id, reset);
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
