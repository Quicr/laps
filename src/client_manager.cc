// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

#include <quicr/detail/defer.h>

#include "client_manager.h"
#include "config.h"
#include "publish_handler.h"
#include "spdlog/fmt/bundled/chrono.h"
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

    std::vector<quicr::ConnectionHandle> ClientManager::UnannounceReceived(quicr::ConnectionHandle connection_handle,
                                           const quicr::TrackNamespace& track_namespace)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_LOGGER_DEBUG(
          LOGGER,
          "Received unannounce from connection handle: {0} for namespace hash: {1}, removing all tracks "
          "associated with namespace",
          connection_handle,
          th.track_namespace_hash);

        // TODO: Fix O(prefix namespaces) matching
        std::vector<quicr::ConnectionHandle> sub_annos_connections;
        for (const auto& [ns, conns] : state_.subscribes_announces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (auto sub_conn_handle : conns) {
                SPDLOG_DEBUG(
                  "Received unannounce matches prefix subscribed from connection handle: {} for namespace hash: {}",
                  sub_conn_handle,
                  th.track_namespace_hash);

                sub_annos_connections.emplace_back(sub_conn_handle);
            }
        }

        for (auto track_alias : state_.announce_active[{track_namespace, connection_handle}]) {
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

        state_.announce_active.erase({ track_namespace, connection_handle });
        peer_manager_.ClientUnannounce({ track_namespace, {}, th.track_fullname_hash });

        return sub_annos_connections;
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
        for (const auto& [key, _] : state_.announce_active) {
            if (key.second == connection_handle) {
                anno_remove_list.push_back(key);
            }
        }

        for (const auto& remove_key : anno_remove_list) {
            state_.announce_active.erase(remove_key);
        }
    }
    void ClientManager::AnnounceReceived(quicr::ConnectionHandle connection_handle,
                                         const quicr::TrackNamespace& track_namespace,
                                         const quicr::PublishAnnounceAttributes& attrs)
    {
        auto th = quicr::TrackHash({ track_namespace, {}, std::nullopt });

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Received announce from connection handle: {} for namespace_hash: {} fullname_hash: {}",
                           connection_handle,
                           th.track_namespace_hash,
                           th.track_fullname_hash);

        // Add to state if not exist
        auto it = state_.announce_active.find({ track_namespace, connection_handle });
        if (it != state_.announce_active.end()) {
            /*
             * @note Connection handle maybe reused. This requires replacing an existing entry if it's duplicate
             */
            PurgePublishState(connection_handle);
        }

        AnnounceResponse announce_response;
        announce_response.reason_code = quicr::Server::AnnounceResponse::ReasonCode::kOk;

        std::vector<quicr::ConnectionHandle> sub_annos_connections;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [ns, conns] : state_.subscribes_announces) {
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

        ResolveAnnounce(connection_handle, track_namespace, sub_annos_connections, announce_response);

        auto& anno_tracks = state_.announce_active[{ track_namespace, connection_handle }];

        // Check if there are any subscribes. If so, send subscribe to announce for all tracks matching namespace
        for (const auto& [ns, sub_tracks]: state_.subscribe_active_) {
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

        /*
         * Always send announcements to peer manager so new clients can trigger subscribe matching and data forwarding
         *    path creation. This needs to be done last to all other client work.
         */
        peer_manager_.ClientAnnounce({ track_namespace, {}, th.track_fullname_hash }, attrs, false);
    }

    ClientManager::SubscribeAnnouncesResponse ClientManager::SubscribeAnnouncesReceived(
      quicr::ConnectionHandle connection_handle,
      const quicr::TrackNamespace& prefix_namespace,
      const quicr::PublishAnnounceAttributes&)
    {
        auto th = quicr::TrackHash({ prefix_namespace, {}, std::nullopt });

        std::cout << "size of subscribe announces " << state_.subscribes_announces.size() << std::endl;
        auto [it, is_new] = state_.subscribes_announces.try_emplace(prefix_namespace);
        it->second.insert(connection_handle);

        if (is_new) {
            SPDLOG_INFO("Subscribe announces received connection handle: {} for namespace_hash: {}, adding to state",
                        connection_handle,
                        th.track_namespace_hash);
        }

        std::vector<quicr::TrackNamespace> matched_ns;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [key, _] : state_.announce_active) {
            // Add matching announced namespaces to vector without duplicates
            if (key.first.HasSamePrefix(prefix_namespace) && (matched_ns.empty() || matched_ns.back() != key.first)) {
                matched_ns.push_back(key.first);
            }
        }

        return { std::nullopt, std::move(matched_ns) };
    }

    void ClientManager::UnsubscribeAnnouncesReceived(quicr::ConnectionHandle connection_handle,
                                                     const quicr::TrackNamespace& prefix_namespace)
    {
        auto it = state_.subscribes_announces.find(prefix_namespace);
        if (it == state_.subscribes_announces.end()) {
            return;
        }

        auto th = quicr::TrackHash({ prefix_namespace, {}, std::nullopt });
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
        for (auto& [ns, conns] : state_.subscribes_announces) {
            auto it = conns.find(connection_handle);
            if (it != conns.end()) {
                conns.erase(it);
                if (conns.empty()) {
                    remove_ns.emplace_back(ns);
                }
            }
        }

        for (auto ns : remove_ns) {
            state_.subscribes_announces.erase(ns);
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

        for (auto& [pub_conn_handle, handler] : sub_it->second.publish_handlers) {
            if (handler != nullptr) {
                UnbindPublisherTrack(pub_conn_handle, handler);
                handler.reset();
            }
        }

        auto& sub_active_list =
          state_.subscribe_active_[{ sub_it->second.track_full_name.name_space, th.track_name_hash }];
        sub_active_list.erase(State::SubscribeInfo{ connection_handle, subscribe_id, th.track_fullname_hash });

        if (sub_active_list.empty()) {
            state_.subscribe_active_.erase({ sub_it->second.track_full_name.name_space, th.track_name_hash });
        }

        state_.subscribes.erase(sub_it);

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
                                          uint64_t proposed_track_alias,
                                          quicr::messages::FilterType filter_type,
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

        if (proposed_track_alias && proposed_track_alias != th.track_fullname_hash) {
            std::ostringstream err;
            err << "Use track alias: " << th.track_fullname_hash;
            ResolveSubscribe(
              connection_handle,
              subscribe_id,
              { quicr::SubscribeResponse::ReasonCode::kRetryTrackAlias, err.str(), th.track_fullname_hash });
            return;
        }

        if (const auto largest = GetLargestAvailable(track_full_name)) {
            ResolveSubscribe(connection_handle,
                             subscribe_id,
                             { quicr::SubscribeResponse::ReasonCode::kOk,
                               std::nullopt,
                               std::nullopt,
                               largest->first,
                               largest->second });
        } else {
            ResolveSubscribe(connection_handle, subscribe_id, { quicr::SubscribeResponse::ReasonCode::kOk });
        }

        ProcessSubscribe(connection_handle, subscribe_id, th, track_full_name, filter_type, attrs);
    }

    quicr::Server::LargestAvailable ClientManager::GetLargestAvailable(const quicr::FullTrackName& track_name)
    {
        // Get the largest object from the cache.
        std::optional<uint64_t> largest_group_id = std::nullopt;
        std::optional<uint64_t> largest_object_id = std::nullopt;

        const auto& th = quicr::TrackHash(track_name);
        const auto cache_entry_it = cache_.find(th.track_fullname_hash);
        if (cache_entry_it != cache_.end()) {
            auto& [_, cache] = *cache_entry_it;
            if (const auto& latest_group = cache.Last(); latest_group && !latest_group->empty()) {
                const auto& latest_object = std::prev(latest_group->end());
                largest_group_id = latest_object->headers.group_id;
                largest_object_id = latest_object->headers.object_id;
            }
        }
        if (!largest_group_id.has_value() || !largest_object_id.has_value()) {
            return std::nullopt;
        }
        return std::make_pair(*largest_group_id, *largest_object_id);
    }

    bool ClientManager::OnFetchOk(quicr::ConnectionHandle connection_handle,
                                  uint64_t subscribe_id,
                                  const quicr::FullTrackName& track_full_name,
                                  const quicr::FetchAttributes& attrs)
    {
        auto pub_fetch_h =
          quicr::PublishFetchHandler::Create(track_full_name, attrs.priority, subscribe_id, attrs.group_order,  config_.object_ttl_);
        BindFetchTrack(connection_handle, pub_fetch_h);

        const auto th = quicr::TrackHash(track_full_name);

        stop_fetch_.try_emplace({ connection_handle, subscribe_id }, false);

        const auto cache_entries = cache_.at(th.track_fullname_hash).Get(attrs.start_group, attrs.end_group + 1);

        if (cache_entries.empty())
            return false;

        std::thread retrieve_cache_thread([=, cache_entries = cache_entries] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

            for (const auto& cache_entry : cache_entries) {
                for (const auto& object : *cache_entry) {
                    if (stop_fetch_[{ connection_handle, subscribe_id }]) {
                        stop_fetch_.erase({ connection_handle, subscribe_id });
                        return;
                    }

                    /*
                     * Stop when reached end group and end object, unless end object is zero. End object of
                     * zero indicates all objects within end group
                     */
                    if (attrs.end_object.has_value() && *attrs.end_object != 0 &&
                        object.headers.group_id == attrs.end_group && object.headers.object_id > *attrs.end_object)
                        break; // Done, reached end object within end group

                    SPDLOG_DEBUG("Fetching group: {} object: {}", object.headers.group_id, object.headers.object_id);

                    pub_fetch_h->PublishObject(object.headers, object.data);
                }
            }
        });

        retrieve_cache_thread.detach();
        return true;
    }

    void ClientManager::FetchCancelReceived(quicr::ConnectionHandle connection_handle,
                                            uint64_t subscribe_id)
    {
        SPDLOG_INFO("Canceling fetch for connection_handle: {} subscribe_id: {}", connection_handle, subscribe_id);

        if (stop_fetch_.count({ connection_handle, subscribe_id }) == 0)
            stop_fetch_[{ connection_handle, subscribe_id }] = true;

    }

    void ClientManager::ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                         uint64_t subscribe_id,
                                         const quicr::TrackHash& th,
                                         const quicr::FullTrackName& track_full_name,
                                         quicr::messages::FilterType filter_type,
                                         const quicr::SubscribeAttributes& attrs)
    {
        bool is_from_peer{ false };
        if (connection_handle == 0 && subscribe_id == 0) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Processing peer subscribe track alias: {} priority: {}", th.track_fullname_hash, attrs.priority);
            is_from_peer = true;
        }

        else {
            SPDLOG_LOGGER_INFO(LOGGER,
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
            state_.subscribe_active_[{ track_full_name.name_space, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle, subscribe_id, th.track_fullname_hash });
            state_.subscribe_alias_sub_id[{ connection_handle, subscribe_id }] = th.track_fullname_hash;

            const auto [_, is_new] = state_.subscribes.try_emplace(
              { th.track_fullname_hash, connection_handle },
              State::SubscribePublishHandlerInfo{
                track_full_name, th.track_fullname_hash, subscribe_id, attrs.priority, attrs.group_order, {} });

            // Always send updates to peers to support subscribe updates and refresh group support
            if (not is_from_peer) {
                quicr::messages::Subscribe sub;
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
        for (auto& [key, track_aliases] : state_.announce_active) {
            if (!key.first.HasSamePrefix(track_full_name.name_space)) {
                continue;
            }

            // if we have already forwarded subscription for the track alias
            // don't forward unless we have expired the refresh period
            if (state_.pub_subscribes.count({ th.track_fullname_hash, key.second }) == 0) {
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
            } else if (filter_type != quicr::messages::FilterType::kLatestGroup) {
                if (not last_subscription_refresh_time.has_value()) {
                    last_subscription_refresh_time = std::chrono::steady_clock::now();
                    continue;
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(now - last_subscription_refresh_time.value())
                    .count();
                if (elapsed > subscription_refresh_interval_ms) {
                    SPDLOG_LOGGER_INFO(
                      LOGGER,
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
