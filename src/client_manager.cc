// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include <quicr/defer.h>

#include "client_manager.h"
#include "config.h"
#include "fetch_handler.h"
#include "publish_handler.h"
#include "subscribe_handler.h"

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
      const quicr::TrackNamespace& track_namespace)
    {
        auto th = quicr::TrackHash({ track_namespace, {} });

        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Received publish namespace done from connection handle: {0} for namespace hash: {1}",
                            connection_handle,
                            th.track_namespace_hash);

        // TODO: Fix O(prefix namespaces) matching
        std::vector<quicr::ConnectionHandle> sub_namespace_connections;
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (auto sub_conn_handle : conns) {
                SPDLOG_DEBUG("Received publish namespace done matches prefix subscribed from connection handle: {} for "
                             "namespace hash: {}",
                             sub_conn_handle,
                             th.track_namespace_hash);

                sub_namespace_connections.emplace_back(sub_conn_handle);
            }
        }

        for (auto track_alias : state_.namespace_active[{ track_namespace, connection_handle }]) {
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

            // Remove publish handler from subscribe
            for (auto it = state_.subscribes.lower_bound({ track_alias, 0 }); it != state_.subscribes.end(); ++it) {
                if (it->first.first != track_alias) {
                    break;
                }

                auto p_it = it->second.publish_handlers.find(connection_handle);
                if (p_it != it->second.publish_handlers.end()) {
                    it->second.publish_handlers.erase(p_it);
                }
            }
        }

        state_.namespace_active.erase({ track_namespace, connection_handle });
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
        for (const auto& [key, _] : state_.namespace_active) {
            if (key.second == connection_handle) {
                anno_remove_list.push_back(key);
            }
        }

        for (const auto& remove_key : anno_remove_list) {
            state_.namespace_active.erase(remove_key);
        }
    }

    void ClientManager::PublishNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                 const quicr::TrackNamespace& track_namespace,
                                                 const quicr::PublishNamespaceAttributes& attrs)
    {

        auto subscribe_to_publisher = [&] {
            auto& anno_tracks = state_.namespace_active[{ track_namespace, connection_handle }];

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
                      sub_ftn, 0, quicr::messages::GroupOrder::kOriginalPublisherOrder, *this);

                    SubscribeTrack(connection_handle, sub_track_handler);
                    state_.pub_subscribes[{ a_si.track_alias, connection_handle }] = sub_track_handler;
                }
            }
        };

        auto th = quicr::TrackHash({ track_namespace, {} });

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Received announce from connection handle: {} for namespace_hash: {} fullname_hash: {}",
                           connection_handle,
                           th.track_namespace_hash,
                           th.track_fullname_hash);

        // Add to state if not exist
        auto it = state_.namespace_active.find({ track_namespace, connection_handle });
        if (it != state_.namespace_active.end()) {
            /*
             * @note Duplicate announce from same connection handle can happen when there are
             *      more than one publish tracks (different name) but use the same namespace.
             *      In this case, we just want to send subscribes
             */
            subscribe_to_publisher();
            return;
        }

        PublishNamespaceResponse announce_response;
        announce_response.reason_code = quicr::Server::PublishNamespaceResponse::ReasonCode::kOk;

        std::vector<quicr::ConnectionHandle> sub_annos_connections;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (auto sub_conn_handle : conns) {
                SPDLOG_DEBUG(
                  "Received announce matches prefix subscribed from connection handle: {} for namespace hash: {}",
                  sub_conn_handle,
                  th.track_namespace_hash);

                sub_annos_connections.emplace_back(sub_conn_handle);
            }
        }

        ResolvePublishNamespace(
          connection_handle, attrs.request_id, track_namespace, sub_annos_connections, announce_response);

        subscribe_to_publisher();

        /*
         * Always send announcements to peer manager so new clients can trigger subscribe matching and data forwarding
         *    path creation. This needs to be done last to all other client work.
         */
        peer_manager_.ClientAnnounce({ track_namespace, {} }, attrs, false);
    }

    void ClientManager::PublishReceived(quicr::ConnectionHandle connection_handle,
                                        uint64_t request_id,
                                        const quicr::messages::PublishAttributes& publish_attributes)
    {
        auto th = quicr::TrackHash(publish_attributes.track_full_name);

        SPDLOG_INFO("Received publish from connection handle: {} using track alias: {} request_id: {}",
                    connection_handle,
                    th.track_fullname_hash,
                    request_id);

        quicr::PublishResponse publish_response;
        publish_response.reason_code = quicr::PublishResponse::ReasonCode::kOk;

        // passively create the subscribe handler towards the publisher
        auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(
          publish_attributes.track_full_name, 0, quicr::messages::GroupOrder::kAscending, *this, true);

        sub_track_handler->SetRequestId(request_id);
        sub_track_handler->SetReceivedTrackAlias(publish_attributes.track_alias);
        sub_track_handler->SetPriority(publish_attributes.priority);

        if (publish_attributes.new_group_request_id.has_value()) {
            sub_track_handler->SupportNewGroupRequest(true);
        }

        SubscribeTrack(connection_handle, sub_track_handler);
        state_.pub_subscribes[{ th.track_fullname_hash, connection_handle }] = sub_track_handler;
        state_.pub_subscribes_by_req_id[{ request_id, connection_handle }] = sub_track_handler;

        quicr::messages::PublishAttributes attrs;
        attrs.is_publisher_initiated = true;
        attrs.priority = publish_attributes.priority;
        attrs.group_order = publish_attributes.group_order;

        ResolvePublish(connection_handle, request_id, attrs, publish_response);

        // Check if there are any subscribers
        bool has_subs{ false };
        for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 }); it != state_.subscribes.end();
             ++it) {
            if (it->first.first == th.track_fullname_hash) {
                has_subs = true;
                break;
            }
        }

        if (not has_subs) {
            SPDLOG_INFO("No subscribers, pause publish connection handle: {0} using track alias: {1}",
                        connection_handle,
                        th.track_fullname_hash);

            sub_track_handler->Pause();
        }

        /*
         * Always send publish as an announcement to peer manager so new clients can trigger subscribe matching and data
         * forwarding path creation. This needs to be done last to all other client work.
         */
        peer_manager_.ClientAnnounce(publish_attributes.track_full_name, {}, false);
    }

    void ClientManager::SubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                   const quicr::TrackNamespace& prefix_namespace,
                                                   const quicr::SubscribeNamespaceAttributes& attributes)
    {
        auto th = quicr::TrackHash({ prefix_namespace, {} });

        std::cout << "size of subscribe namespace " << state_.subscribes_namespaces.size() << std::endl;
        auto [it, is_new] = state_.subscribes_namespaces.try_emplace(prefix_namespace);
        it->second.insert(connection_handle);

        if (is_new) {
            SPDLOG_INFO("Subscribe namespace received connection handle: {} for namespace_hash: {}, adding to state",
                        connection_handle,
                        th.track_namespace_hash);
        }

        std::vector<quicr::TrackNamespace> matched_ns;
        std::vector<quicr::SubscribeNamespaceResponse::AvailableTrack> matched_tracks;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [key, _] : state_.namespace_active) {
            // Add matching announced namespaces to vector without duplicates
            if (key.first.HasSamePrefix(prefix_namespace) && (matched_ns.empty() || matched_ns.back() != key.first)) {
                matched_ns.push_back(key.first);
            }
        }

        // TODO: Need to change this to use what peering is using to prefix match instead of O(n) over all publish
        //  subscribes
        for (const auto& [ta_conn, handler] : state_.pub_subscribes) {
            if (ta_conn.second == connection_handle || !handler)
                continue;

            const auto& track_full_name = handler->GetFullTrackName();
            const bool ns_matched = prefix_namespace.HasSamePrefix(track_full_name.name_space);
            if (ns_matched) {
                std::optional<quicr::messages::Location> largest_location = GetLargestAvailable(track_full_name);

                quicr::messages::PublishAttributes publish_attributes;
                publish_attributes.track_alias = ta_conn.first;
                publish_attributes.priority = handler->GetPriority(); // Original priority?
                publish_attributes.group_order = handler->GetGroupOrder();
                publish_attributes.delivery_timeout = handler->GetDeliveryTimeout();
                publish_attributes.filter_type = handler->GetFilterType();
                publish_attributes.forward = true;
                publish_attributes.new_group_request_id = std::nullopt;
                publish_attributes.is_publisher_initiated = true;
                matched_tracks.emplace_back(track_full_name, largest_location, publish_attributes);

                SPDLOG_LOGGER_INFO(
                  LOGGER,
                  "Matched PUBLISH track for SUBSCRIBE_NAMESPACE: conn: {} track_alias: {} track_hash: {}",
                  connection_handle,
                  ta_conn.first,
                  quicr::TrackHash(track_full_name).track_fullname_hash);
            }
        }

        const quicr::SubscribeNamespaceResponse response = { .reason_code =
                                                               quicr::SubscribeNamespaceResponse::ReasonCode::kOk,
                                                             .tracks = matched_tracks,
                                                             .namespaces = std::move(matched_ns) };
        ResolveSubscribeNamespace(connection_handle, attributes.request_id, prefix_namespace, response);
    }

    void ClientManager::UnsubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                                     const quicr::TrackNamespace& prefix_namespace)
    {
        auto it = state_.subscribes_namespaces.find(prefix_namespace);
        if (it == state_.subscribes_namespaces.end()) {
            return;
        }

        auto th = quicr::TrackHash({ prefix_namespace, {} });
        SPDLOG_INFO("Unsubscribe announces received connection handle: {} for namespace_hash: {}, removing",
                    connection_handle,
                    th.track_namespace_hash);
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
                conns.erase(it);
                if (conns.empty()) {
                    remove_ns.emplace_back(ns);
                }
            }
        }

        for (auto ns : remove_ns) {
            state_.subscribes_namespaces.erase(ns);
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
            SPDLOG_WARN("Unable to find subscribe by request id for connection handle: {0} request_id: {1}",
                        connection_handle,
                        request_id);
            return;
        }

        std::lock_guard<std::mutex> _(state_.state_mutex);

        auto th = quicr::TrackHash(s_it->second->GetFullTrackName());
        state_.pub_subscribes.erase({ th.track_fullname_hash, connection_handle });
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

        for (auto& [pub_conn_handle, handler] : sub_it->second.publish_handlers) {
            if (handler != nullptr) {
                UnbindPublisherTrack(connection_handle, pub_conn_handle, handler);
                handler.reset();
            }
        }

        auto& sub_active_list =
          state_.subscribe_active_[{ sub_it->second.track_full_name.name_space, th.track_name_hash }];
        sub_active_list.erase(State::SubscribeInfo{ connection_handle, request_id, th.track_fullname_hash });

        if (sub_active_list.empty()) {
            state_.subscribe_active_.erase({ sub_it->second.track_full_name.name_space, th.track_name_hash });
        }

        state_.subscribes.erase(sub_it);

        RemoveOrPausePublisherSubscribe(th);
    }

    void ClientManager::RemoveOrPausePublisherSubscribe(const quicr::TrackHash& track_hash)
    {
        // Do nothing if peering still has a subscriber
        if (peer_manager_.HasSubscribers(track_hash.track_fullname_hash)) {
            return;
        }

        // Do nothing if there is still one direct client subscribe
        for (auto it = state_.subscribes.lower_bound({ track_hash.track_fullname_hash, 0 });
             it != state_.subscribes.end();
             ++it) {
            if (it->first.first == track_hash.track_fullname_hash) {
                return;
            }
        }

        peer_manager_.ClientUnsubscribe(track_hash.track_fullname_hash);

        SPDLOG_LOGGER_INFO(
          LOGGER, "No subscribers left, unsubscribe publisher track_alias: {0}", track_hash.track_fullname_hash);

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

            if (sub_to_pub_handler->IsPublisherInitiated()) {
                sub_to_pub_handler->Pause();
            } else {
                UnsubscribeTrack(key.second, sub_to_pub_handler);
                remove_sub_pub.push_back(key);
            }
        }

        for (const auto& key : remove_sub_pub) {
            state_.pub_subscribes.erase(key);
        }
    }

    void ClientManager::TrackStatusReceived(quicr::ConnectionHandle connection_handle,
                                            uint64_t request_id,
                                            const quicr::FullTrackName& track_full_name,
                                            const quicr::messages::SubscribeAttributes& subscribe_attributes)
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
                                   th.track_fullname_hash,
                                   {
                                     quicr::SubscribeResponse::ReasonCode::kOk,
                                     it->second->IsPublisherInitiated(),
                                     std::nullopt,
                                     largest,
                                   });
                return;
            }
        }

        ResolveTrackStatus(connection_handle,
                           request_id,
                           th.track_fullname_hash,
                           {
                             quicr::SubscribeResponse::ReasonCode::kTrackDoesNotExist,
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

        SPDLOG_LOGGER_INFO(LOGGER,
                           "New subscribe connection handle: {0} request_id: {1} track alias: {2} priority: {3}",
                           connection_handle,
                           request_id,
                           th.track_fullname_hash,
                           attrs.priority);

        if (const auto largest = GetLargestAvailable(track_full_name)) {
            ResolveSubscribe(
              connection_handle,
              request_id,
              th.track_fullname_hash,
              { quicr::SubscribeResponse::ReasonCode::kOk, attrs.is_publisher_initiated, std::nullopt, largest });
        } else {
            ResolveSubscribe(
              connection_handle, request_id, th.track_fullname_hash, { quicr::SubscribeResponse::ReasonCode::kOk });
        }

        ProcessSubscribe(connection_handle, request_id, th, track_full_name, attrs);
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
                                      quicr::messages::SubscriberPriority priority,
                                      quicr::messages::GroupOrder group_order,
                                      quicr::messages::Location start,
                                      std::optional<quicr::messages::Location> end)
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
        }

        if (start.group > end->group || largest_location.value().group < start.group) {
            reason_code = quicr::FetchResponse::ReasonCode::kInvalidRange;
        }

        const auto& cache_entries =
          cache_entry_it->second.Get(start.group, end->group != 0 ? end->group : cache_entry_it->second.Size());

        if (cache_entries.empty()) {
            reason_code = quicr::FetchResponse::ReasonCode::kNoObjects;
        }

        // TODO: Adjust the TTL to allow more time for transmission
        auto pub_fetch_h =
          quicr::PublishFetchHandler::Create(track_full_name, priority, request_id, group_order, config_.object_ttl_);
        BindFetchTrack(connection_handle, pub_fetch_h);

        stop_fetch_.try_emplace({ connection_handle, request_id }, false);

        if (!end.has_value()) {
            end = { 0, 0 };
        }

        reason_code = quicr::FetchResponse::ReasonCode::kInternalError;

        SPDLOG_LOGGER_DEBUG(LOGGER, "Fetch received conn_id: {} request_id: {}", connection_handle, request_id);
        std::thread retrieve_cache_thread([=, cache_entries = std::move(cache_entries), this] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

            auto rc = reason_code;

            if (rc != quicr::FetchResponse::ReasonCode::kOk) {
                // Try to see if original publisher can provide the data
                auto track_handler = FetchTrackHandler::Create(pub_fetch_h,
                                                               track_full_name,
                                                               priority,
                                                               group_order,
                                                               start.group,
                                                               end->group,
                                                               start.object,
                                                               end->object);

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

                    if (end->object && object.headers.group_id == end->group &&
                        object.headers.object_id >= end->object) {
                        return;
                    }

                    SPDLOG_TRACE("Fetching group: {} object: {}", object.headers.group_id, object.headers.object_id);
                    pub_fetch_h->PublishObject(object.headers, object.data);
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
        uint64_t joining_start = 0;

        if (attributes.relative) {
            if (const auto largest = GetLargestAvailable(track_full_name)) {
                if (largest->group > attributes.joining_start)
                    joining_start = largest->group - attributes.joining_start;
            }
        } else {
            joining_start = attributes.joining_start;
        }

        FetchReceived(connection_handle,
                      request_id,
                      track_full_name,
                      attributes.priority,
                      attributes.group_order,
                      { joining_start, 0 },
                      std::nullopt);
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
                                              quicr::messages::FilterType::kLargestObject,
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

            if (!(it->second->GetPendingNewRquestId().has_value() && *it->second->GetPendingNewRquestId() == 0 &&
                  group_id == 0) &&
                (group_id == 0 || it->second->GetLatestGroupId() < group_id)) {

                it->second->SetNewGroupRequestId(group_id);
                DampenOrUpdateTrackSubscription(it->second, true);
            }
        }
    }

    bool ClientManager::DampenOrUpdateTrackSubscription(std::shared_ptr<SubscribeTrackHandler> sub_to_pub_track_handler,
                                                        bool new_group_request)
    {
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
                                         const quicr::messages::SubscribeAttributes& attrs)
    {
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
                               "{} ns: {} name: {} new_group_request: {}",
                               connection_handle,
                               request_id,
                               th.track_fullname_hash,
                               attrs.priority,
                               th.track_namespace_hash,
                               th.track_name_hash,
                               attrs.new_group_request_id.has_value() ? *attrs.new_group_request_id : -1);

            // record subscribe as active from this subscriber
            state_.subscribe_active_[{ track_full_name.name_space, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle, request_id, th.track_fullname_hash });
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
            quicr::messages::Subscribe sub(
              request_id,
              track_full_name.name_space,
              track_full_name.name,
              attrs.priority,
              attrs.group_order,
              true,
              quicr::messages::FilterType::kLargestObject, // Filters are only for edge to apply
              std::nullopt,
              std::nullopt,
              {});

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

            DampenOrUpdateTrackSubscription(it->second, attrs.new_group_request_id.has_value());
        }

        // Subscribe to announcer if announcer is active
        for (auto& [key, track_aliases] : state_.namespace_active) {
            if (!key.first.HasSamePrefix(track_full_name.name_space)) {
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
                                                          *this);
                SubscribeTrack(key.second, sub_track_h);

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
