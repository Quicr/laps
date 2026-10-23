// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <quicr/handlers/subscribe_track_handler.h>
#include <quicr/session.h>

#include <quicr/utilities/defer.h>

#include "client_manager.h"
#include "config.h"
#include "peering_bridge.h"
#include "fetch_handler.h"
#include "publish_handler.h"
#include "publish_namespace_handler.h"
#include "subscribe_handler.h"
#include "transport_helpers.h"

#include <ranges>

namespace laps {
    ClientManager::ClientManager(State& state,
                                 const Config& config,
                                 const quicr::ServerConfig& cfg,
                                 moq::peering::PeerManager& peer_manager,
                                 size_t cache_duration_ms)
      : state_(state)
      , config_(config)
      , server_config_(cfg)
      , peer_manager_(peer_manager)
      , tick_service_(config.tick_service_)
      , metrics_publisher_(*this, config)
      , cache_duration_ms_(cache_duration_ms)
    {
    }

    ClientManager::~ClientManager()
    {
        Stop();
    }

    quicr::Session::Status ClientManager::Start()
    {
        /*
         * Set up relay-local metrics track state before the server transport is listening. The transport
         * thread dispatches all control/data callbacks; starting metrics publishing first avoids any chance
         * of racing with that thread while touching ClientManager state.
         */
        metrics_publisher_.Start();

        session_manager_ = std::make_unique<quicr::SessionManager>(
          std::static_pointer_cast<quicr::SessionManager::Callbacks>(shared_from_this()),
          tick_service_,
          config_.quicr_logger_);

        session_manager_->AddTransport(server_config_,
                                       std::static_pointer_cast<quicr::Session::ServerCallbacks>(shared_from_this()));

        return quicr::Session::Status::kReady;
    }

    void ClientManager::Stop()
    {
        /*
         * Drop the peer delegate before the transport goes away so new peer callbacks do not enter relay
         * state that is about to be released. The session manager is next so its thread stops dispatching.
         */
        peer_manager_.SetDelegate(nullptr);

        session_manager_.reset();

        metrics_publisher_.Stop();

        {
            std::lock_guard<std::mutex> _(sessions_mutex_);
            sessions_.clear();
        }

        ReleaseTrackedHandlers();
    }

    void ClientManager::ReleaseTrackedHandlers()
    {
        std::vector<std::shared_ptr<SubscribeTrackHandler>> subscribe_handlers;
        std::vector<std::shared_ptr<PublishNamespaceHandler>> namespace_handlers;

        {
            std::lock_guard<std::mutex> _(state_.state_mutex);

            for (auto& [_, handler] : state_.pub_subscribes) {
                if (handler) {
                    subscribe_handlers.push_back(std::move(handler));
                }
            }
            state_.pub_subscribes.clear();

            for (auto& [_, handler] : state_.pub_subscribes_by_req_id) {
                if (handler) {
                    subscribe_handlers.push_back(std::move(handler));
                }
            }
            state_.pub_subscribes_by_req_id.clear();

            for (auto& [_, connections] : state_.subscribe_tracks) {
                for (auto& [_, handler] : connections) {
                    if (handler) {
                        namespace_handlers.push_back(std::move(handler));
                    }
                }
            }
            state_.subscribe_tracks.clear();
        }

        // Destructors unbind subscriber publish handlers. sessions_mutex_ is still alive, and the
        // sessions themselves are already gone, so the unbind finds nothing to send.
        subscribe_handlers.clear();
        namespace_handlers.clear();
    }

    std::shared_ptr<quicr::Session> ClientManager::GetSession(std::uint64_t connection_handle) const
    {
        std::lock_guard<std::mutex> _(sessions_mutex_);

        const auto it = sessions_.find(connection_handle);
        if (it == sessions_.end()) {
            return nullptr;
        }

        return it->second.lock();
    }

    void ClientManager::NewConnectionAccepted(const std::shared_ptr<quicr::Session>& session)
    {
        const auto connection_handle = ConnectionHandle(session);

        {
            std::lock_guard<std::mutex> _(sessions_mutex_);
            sessions_[connection_handle] = session;
        }

        // Peer address requires the owning transport; SessionManager does not expose it. See libquicr issue.
        const auto [ip, port] = GetPeerAddress(nullptr, session->GetConnection());
        metrics_publisher_.AddConnection(connection_handle, { ip, port });

        SPDLOG_LOGGER_INFO(LOGGER, "New connection handle {0} accepted from {1}:{2}", connection_handle, ip, port);
    }

    void ClientManager::OnSessionRemoved(const std::shared_ptr<quicr::Session>& session)
    {
        const auto connection_handle = ConnectionHandle(session);

        /*
         * Drop the session first so that every GetSession() lookup for this handle now fails. Relay teardown
         * below walks state that still references the connection, and without this the teardown would call
         * back into a session that libquicr is closing on another thread.
         */
        {
            std::lock_guard<std::mutex> _(sessions_mutex_);

            if (sessions_.erase(connection_handle) == 0) {
                return;
            }
        }

        SPDLOG_LOGGER_DEBUG(LOGGER, "Connection closed; connection_handle: {0} ", connection_handle);

        metrics_publisher_.RemoveConnection(connection_handle);

        // Remove all subscribe announces for this connection handle
        std::vector<quicr::TrackNamespace> remove_ns;
        for (auto& [ns, conns] : state_.subscribe_tracks) {
            auto it = conns.find(connection_handle);
            if (it != conns.end()) {
                remove_ns.emplace_back(ns);
            }
        }

        for (auto ns : remove_ns) {
            UnsubscribeNamespaceReceived(session, ns);
        }

        std::vector<quicr::TrackNamespace> remove_sub_ns;
        for (const auto& [ns, conns] : state_.subscribe_namespaces) {
            if (conns.find(connection_handle) != conns.end()) {
                remove_sub_ns.push_back(ns);
            }
        }
        for (const auto& ns : remove_sub_ns) {
            DropSubscribeNamespace(connection_handle, ns);
        }

        // Clean up subscribe states
        std::vector<std::pair<std::uint64_t, std::uint64_t>> unsub_list;
        for (auto it = state_.subscribe_alias_req_id.lower_bound({ connection_handle, 0 });
             it != state_.subscribe_alias_req_id.end();
             it++) {
            const auto& [key, _] = *it;
            if (key.first != connection_handle)
                break;

            unsub_list.push_back(key);
        }

        for (auto& key : unsub_list) {
            UnsubscribeReceived(session, key.second);
        }

        // Cleanup publish states
        PurgePublishState(connection_handle);

        // Drop any namespaces that were fanned out to this connection
        for (auto it = fanout_namespaces_.begin(); it != fanout_namespaces_.end();) {
            it = it->first.second == connection_handle ? fanout_namespaces_.erase(it) : std::next(it);
        }
    }

    // -------------------------------------------------------------------------------
    // Session API by connection handle
    // -------------------------------------------------------------------------------

    void ClientManager::SubscribeTrack(std::uint64_t connection_handle,
                                       std::shared_ptr<quicr::SubscribeTrackHandler> handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->SubscribeTrack(std::move(handler));
        }
    }

    void ClientManager::UnsubscribeTrack(std::uint64_t connection_handle,
                                         const std::shared_ptr<quicr::SubscribeTrackHandler>& handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->UnsubscribeTrack(handler);
        }
    }

    void ClientManager::UpdateTrackSubscription(std::uint64_t connection_handle,
                                                std::shared_ptr<quicr::SubscribeTrackHandler> handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->UpdateTrackSubscription(std::move(handler));
        }
    }

    void ClientManager::PublishNamespace(std::uint64_t connection_handle,
                                         std::shared_ptr<quicr::PublishNamespaceHandler> handler,
                                         bool passive)
    {
        if (auto session = GetSession(connection_handle)) {
            session->PublishNamespace(std::move(handler), passive);
        }
    }

    void ClientManager::FetchTrack(std::uint64_t connection_handle, std::shared_ptr<quicr::FetchTrackHandler> handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->FetchTrack(std::move(handler));
        }
    }

    void ClientManager::CancelFetchTrack(std::uint64_t connection_handle,
                                         std::shared_ptr<quicr::FetchTrackHandler> handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->CancelFetchTrack(std::move(handler));
        }
    }

    void ClientManager::BindPublisherTrack(std::uint64_t connection_handle,
                                           std::uint64_t src_id,
                                           uint64_t request_id,
                                           const std::shared_ptr<quicr::PublishTrackHandler>& handler,
                                           bool ephemeral)
    {
        if (auto session = GetSession(connection_handle)) {
            session->BindPublisherTrack(src_id, request_id, handler, ephemeral);
        }
    }

    void ClientManager::UnbindPublisherTrack(std::uint64_t connection_handle,
                                             std::uint64_t src_id,
                                             const std::shared_ptr<quicr::PublishTrackHandler>& handler,
                                             bool send_publish_done)
    {
        if (auto session = GetSession(connection_handle)) {
            session->UnbindPublisherTrack(src_id, handler, send_publish_done);
        }
    }

    void ClientManager::BindFetchTrack(std::uint64_t connection_handle,
                                       std::shared_ptr<quicr::PublishFetchHandler> handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->BindFetchTrack(std::move(handler));
        }
    }

    void ClientManager::UnbindFetchTrack(std::uint64_t connection_handle,
                                         const std::shared_ptr<quicr::PublishFetchHandler>& handler)
    {
        if (auto session = GetSession(connection_handle)) {
            session->UnbindFetchTrack(handler);
        }
    }

    // -------------------------------------------------------------------------------
    // SessionManager lifecycle
    // -------------------------------------------------------------------------------

    void ClientManager::OnNewServerSession(const std::shared_ptr<quicr::Session>& session)
    {
        NewConnectionAccepted(session);
    }

    // -------------------------------------------------------------------------------
    // Session request callbacks
    // -------------------------------------------------------------------------------

    std::uint64_t ClientManager::ConnectionHandle(const std::shared_ptr<quicr::Session>& session)
    {
        if (session == nullptr) {
            return 0;
        }

        const auto& connection = session->GetConnection();
        return connection != nullptr ? connection->GetID() : 0;
    }

    quicr::Reply<void, quicr::ErrorCode> ClientManager::NewGroupRequested(const quicr::FullTrackName& track_full_name,
                                                                          std::uint64_t group_id)
    {
        auto th = quicr::TrackHash(track_full_name);
        SPDLOG_INFO("New group requested received track_alais: {} group_id: {} ", th.track_fullname_hash, group_id);

        // Update peering subscribe info - This will update existing instead of creating new
        quicr::SubscribeAttributes update_attrs;
        update_attrs.priority = kDefaultPriority;
        update_attrs.delivery_timeout = std::chrono::milliseconds(kDefaultObjectTtl);
        update_attrs.expires = std::chrono::milliseconds(0);
        update_attrs.forward = 1;
        update_attrs.new_group_request_id = group_id;
        peer_manager_.UpdateSubscribe(ToPeeringTrack(track_full_name), ToPeeringParams(update_attrs));

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

        return {};
    }

    void ClientManager::FanoutPublishNamespace(const quicr::TrackNamespace& track_namespace,
                                               std::uint64_t exclude_connection)
    {
        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [prefix, conns] : state_.subscribe_tracks) {
            if (!prefix.HasSamePrefix(track_namespace)) {
                continue;
            }

            for (const auto connection_handle : std::views::keys(conns)) {
                if (connection_handle == exclude_connection) {
                    continue;
                }

                auto session = GetSession(connection_handle);
                if (session == nullptr) {
                    continue;
                }

                auto [it, is_new] = fanout_namespaces_.try_emplace({ track_namespace, connection_handle });
                if (!is_new) {
                    continue;
                }

                it->second = quicr::PublishNamespaceHandler::Create(track_namespace);
                session->PublishNamespace(it->second);

                SPDLOG_LOGGER_DEBUG(LOGGER,
                                    "Publish namespace fanned out to prefix subscriber connection handle: {} for "
                                    "namespace hash: {}",
                                    connection_handle,
                                    quicr::TrackHash({ track_namespace, {} }).track_namespace_hash);
            }
        }
    }

    void ClientManager::FanoutPublishNamespaceDone(const quicr::TrackNamespace& track_namespace)
    {
        for (auto it = fanout_namespaces_.lower_bound({ track_namespace, 0 }); it != fanout_namespaces_.end();) {
            if (it->first.first != track_namespace) {
                break;
            }

            if (auto session = GetSession(it->first.second)) {
                session->PublishNamespaceDone(it->second);
            }

            it = fanout_namespaces_.erase(it);
        }
    }

    void ClientManager::NotifySubscribeNamespaces(const quicr::TrackNamespace& track_namespace,
                                                  std::uint64_t exclude_connection,
                                                  bool done)
    {
        for (const auto& [prefix, connections] : state_.subscribe_namespaces) {
            const auto match = prefix.IsPrefixOf(track_namespace);
            if (match == std::partial_ordering::unordered || match == std::partial_ordering::greater) {
                continue;
            }

            for (const auto connection_handle : connections) {
                if (connection_handle == exclude_connection && !config_.allow_self) {
                    continue;
                }

                const auto key = std::make_pair(track_namespace, connection_handle);
                if (done) {
                    if (notified_namespaces_.erase(key) == 0) {
                        continue;
                    }
                } else if (!notified_namespaces_.emplace(key).second) {
                    continue;
                }

                auto session = GetSession(connection_handle);
                if (session == nullptr) {
                    if (!done) {
                        notified_namespaces_.erase(key);
                    }
                    continue;
                }

                if (done) {
                    session->SendNamespaceDone(track_namespace);
                } else {
                    session->SendNamespace(track_namespace);
                }

                SPDLOG_LOGGER_DEBUG(LOGGER,
                                    "{} subscribe namespace connection handle: {} namespace hash: {}",
                                    done ? "NAMESPACE_DONE to" : "NAMESPACE to",
                                    connection_handle,
                                    quicr::TrackHash({ track_namespace, {} }).track_namespace_hash);
            }
        }
    }

    bool ClientManager::NamespaceStillPublished(const quicr::TrackNamespace& track_namespace) const
    {
        for (const auto& [key, _] : state_.pub_namespace_active) {
            if (key.first == track_namespace) {
                return true;
            }
        }

        for (const auto& [_, handler] : state_.pub_subscribes) {
            if (handler != nullptr && handler->GetFullTrackName().name_space == track_namespace) {
                return true;
            }
        }

        return false;
    }

    void ClientManager::DropSubscribeNamespace(std::uint64_t connection_handle, const quicr::TrackNamespace& prefix)
    {
        const auto it = state_.subscribe_namespaces.find(prefix);
        if (it == state_.subscribe_namespaces.end() || it->second.erase(connection_handle) == 0) {
            return;
        }

        if (it->second.empty()) {
            state_.subscribe_namespaces.erase(it);
        }

        for (auto notified = notified_namespaces_.begin(); notified != notified_namespaces_.end();) {
            const auto match = prefix.IsPrefixOf(notified->first);
            if (notified->second == connection_handle &&
                (match == std::partial_ordering::less || match == std::partial_ordering::equivalent)) {
                notified = notified_namespaces_.erase(notified);
            } else {
                ++notified;
            }
        }
    }

    void ClientManager::PublishNamespaceDoneReceived(const std::shared_ptr<quicr::Session>& session,
                                                     std::uint64_t request_id)
    {
        const auto connection_handle = ConnectionHandle(session);

        auto it = state_.requests.find({ connection_handle, request_id });
        if (it == state_.requests.end()) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER,
              "Received publish namespace done from connection handle: {0} request_id: {1} but request Id not in state",
              connection_handle,
              request_id);

            return;
        }

        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Received publish namespace done from connection handle: {0} request_id: {1}",
                            connection_handle,
                            request_id);

        auto& track_namespace = std::any_cast<quicr::TrackNamespace&>(it->second.related_data);
        auto th = quicr::TrackHash({ track_namespace, {} });

        peer_manager_.Withdraw(ToPeeringTrack({ track_namespace, {} }), moq::peering::Announcement::kNamespace);

        FanoutPublishNamespaceDone(track_namespace);

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

        if (!NamespaceStillPublished(track_namespace)) {
            NotifySubscribeNamespaces(track_namespace, connection_handle, true);
        }

        if (connection_handle)
            peer_manager_.Withdraw(ToPeeringTrack({ track_namespace, {} }), moq::peering::Announcement::kTrack);
    }

    void ClientManager::PurgePublishState(std::uint64_t connection_handle)
    {
        // Snapshot of tracks to remove from top-n.
        std::vector<std::shared_ptr<SubscribeTrackHandler>> top_n_removal;

        {
            std::lock_guard<std::mutex> _(state_.state_mutex);

            // Collect publisher subscriptions for this connection.
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pub_subs;
            for (const auto& [key, _] : state_.pub_subscribes) {
                if (key.second == connection_handle) {
                    pub_subs.push_back(key);
                }
            }

            for (const auto& remove_key : pub_subs) {
                auto handler = state_.pub_subscribes.at(remove_key);
                state_.pub_subscribes.erase(remove_key);
                SPDLOG_LOGGER_DEBUG(LOGGER,
                                    "Purge publish state for track hash: {} connection handle: {}",
                                    remove_key.first,
                                    remove_key.second);

                // Remove from top-n if no other connection publishes this track.
                const auto next = state_.pub_subscribes.lower_bound({ remove_key.first, 0 });
                if (next == state_.pub_subscribes.end() || next->first.first != remove_key.first) {
                    top_n_removal.emplace_back(std::move(handler));
                }
            }

            // Clear pub_subscribes_by_req_id for this connection.
            for (auto it = state_.pub_subscribes_by_req_id.begin(); it != state_.pub_subscribes_by_req_id.end();) {
                if (it->first.second != connection_handle) {
                    ++it;
                    continue;
                }
                it = state_.pub_subscribes_by_req_id.erase(it);
            }

            std::vector<std::pair<quicr::TrackNamespace, std::uint64_t>> anno_remove_list;
            for (const auto& [key, _] : state_.pub_namespace_active) {
                if (key.second == connection_handle) {
                    anno_remove_list.push_back(key);
                }
            }

            for (const auto& remove_key : anno_remove_list) {
                state_.pub_namespace_active.erase(remove_key);
            }
        }

        // Now remove these tracks from the top-n ranking outside the lock.
        for (const auto& handler : top_n_removal) {
            if (handler) {
                handler->RemoveFromTrackRanking();
            }
        }
    }

    quicr::Reply<void, quicr::PublishNamespaceErrorCode> ClientManager::PublishNamespaceReceived(
      const std::shared_ptr<quicr::Session>& session,
      const quicr::TrackNamespace& track_namespace,
      const quicr::PublishNamespaceAttributes& attrs)
    {
        const auto connection_handle = ConnectionHandle(session);

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
                    auto sub_track_handler =
                      std::make_shared<SubscribeTrackHandler>(sub_ftn, 0, std::nullopt, *this, config_.tick_service_);

                    // Add subscribers to publisher subscribe handler
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
            return {};
        }

        state_.pub_namespace_active.try_emplace(std::make_pair(track_namespace, connection_handle));

        /*
         * The announcing connection is acknowledged by libquicr once this callback returns, which also mirrors
         * the namespace back to a matching prefix subscription on that same connection. Every other prefix
         * subscriber is the relay's own work.
         */
        FanoutPublishNamespace(track_namespace, connection_handle);

        if (connection_handle) {
            subscribe_to_publisher();

            /*
             * Always send announcements to peer manager so new clients can trigger subscribe matching and data
             * forwarding path creation. This needs to be done last to all other client work.
             */
            peer_manager_.Announce(ToPeeringTrack({ track_namespace, {} }), moq::peering::Announcement::kNamespace);
        }

        NotifySubscribeNamespaces(track_namespace, connection_handle, false);

        return {};
    }

    quicr::Reply<const quicr::PublishResponse, quicr::PublishErrorCode> ClientManager::PublishReceived(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id,
      const quicr::PublishAttributes& publish_attributes,
      [[maybe_unused]] std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler)
    {
        return PublishReceivedInternal(
          session, request_id, publish_attributes, !ConnectionHandle(session) && !request_id);
    }

    void ClientManager::RegisterLocalPublish(const quicr::PublishAttributes& publish_attributes)
    {
        PublishReceivedInternal(nullptr, 0, publish_attributes, false);
    }

    quicr::Reply<const quicr::PublishResponse, quicr::PublishErrorCode> ClientManager::PublishReceivedInternal(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id,
      const quicr::PublishAttributes& publish_attributes,
      bool is_from_peer)
    {
        std::unique_lock lock(state_.state_mutex);
        const auto connection_handle = ConnectionHandle(session);

        /*
         * Peer-originated publishes and relay-local publishes (e.g. the metrics track) both use the
         * connection_handle=0/request_id=0 sentinel because there is no real connection to resolve the
         * publish response against. Track alias assignment and ResolvePublish()/ClientAnnounce() must be
         * bypassed for both, independent of whether the publish is actually from-peer.
         */
        const bool bypass_resolve = !connection_handle && !request_id;

        auto th = quicr::TrackHash(publish_attributes.track_full_name);

        SPDLOG_LOGGER_INFO(
          LOGGER,
          "Received publish from connection handle: {} using track alias: {} request_id: {} tfn: {} ({})",
          connection_handle,
          th.track_fullname_hash,
          request_id,
          publish_attributes.track_full_name.NamespaceStr(),
          publish_attributes.track_full_name.NameStr());

        // PublishTrack within publish namespace handler if matched
        for (auto& [tn, conns] : state_.subscribe_tracks) {
            const auto prefix_match = tn.IsPrefixOf(publish_attributes.track_full_name.name_space);

            if (prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) {

                SPDLOG_LOGGER_DEBUG(LOGGER,
                                    "Publish matches subscribe namespace track alias: {} request_id: {} tfn: {} ({})",
                                    th.track_fullname_hash,
                                    request_id,
                                    publish_attributes.track_full_name.NamespaceStr(),
                                    publish_attributes.track_full_name.NameStr());

                for (auto& [_, pub_ns_h] : conns) {
                    if (!config_.allow_self && pub_ns_h->GetConnectionId() == connection_handle) {
                        // Initially do not mirror
                        continue;
                    }

                    const auto handler = PublishTrackHandler::Create(
                      publish_attributes.track_full_name,
                      quicr::TrackMode::kStream,
                      publish_attributes.publisher_priority,
                      publish_attributes.publisher_priority,
                      publish_attributes.delivery_timeout.value_or(config_.object_ttl_),
                      publish_attributes.largest_object.value_or(quicr::messages::Location{ 0, 0 }),
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
        sub_track_handler->SetSubscriberPriority(publish_attributes.publisher_priority);

        if (bypass_resolve) {
            // Accepting the publish normally binds the handler to its connection, which is what assigns the
            // track alias. There is no connection here, so set it explicitly.
            sub_track_handler->SetTrackAlias(publish_attributes.track_alias);
        }

        if (is_from_peer) {
            sub_track_handler->SetFromPeer();
        }

        if (publish_attributes.dynamic_groups) {
            sub_track_handler->SupportNewGroupRequest(true);
        }

        /*
         * libquicr attaches the session only after this callback returns. The handler is visible to
         * OnPeerSubscribe as soon as it is in pub_subscribes, and that callback sends on the session.
         * Bind the session first. Announce below drops this lock, so the peer thread can proceed.
         */
        if (session) {
            sub_track_handler->SetRequestId(request_id);
            sub_track_handler->SetReceivedTrackAlias(publish_attributes.track_alias);
            sub_track_handler->SetDeliveryTimeout(
              std::chrono::milliseconds(publish_attributes.delivery_timeout.value_or(config_.object_ttl_)));
            sub_track_handler->SetPublisherDefaultGroupOrder(publish_attributes.default_publisher_group_order);
            session->SubscribeTrack(sub_track_handler);
        }

        state_.pub_subscribes[{ th.track_fullname_hash, connection_handle }] = sub_track_handler;

        if (!bypass_resolve) {
            state_.pub_subscribes_by_req_id[{ request_id, connection_handle }] = sub_track_handler;

            /*
             * Always send publish as an announcement to peer manager so new clients can trigger subscribe matching and
             * data forwarding path creation. This needs to be done last to all other client work.
             */
            lock.unlock();
            peer_manager_.Announce(ToPeeringTrack(publish_attributes.track_full_name), moq::peering::Announcement::kTrack);
            lock.lock();
        }

        // Check if there are any subscribers
        bool has_peer_subs{ peer_manager_.HasSubscribers(th.track_fullname_hash) };
        bool has_subs{ has_peer_subs };

        if (has_peer_subs) {
            // libquicr copies the publisher's timeout onto the handler only after this callback returns, and
            // the largest object is optional on the publish. A peer that is already subscribed hits this
            // before either value exists.
            sub_track_handler->AddSubscriber(
              0,
              0,
              sub_track_handler->GetSubscriberPriority(),
              std::chrono::milliseconds(publish_attributes.delivery_timeout.value_or(config_.object_ttl_)),
              publish_attributes.largest_object.value_or(quicr::messages::Location{ 0, 0 }));
        }

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

        quicr::TrackNamespace sub_ns;
        for (const auto& [ns_prefix, conns] : state_.subscribe_tracks) {
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

        /*
         * Accept the publish, but only ask the publisher to forward data if the relay has somewhere to send it.
         * The handler is not bound to the connection until the accept is processed, so Pause() cannot send the
         * update itself; answering the publish with forward off does the same thing in one less message. The
         * handler is still marked paused so that gaining a subscriber resumes forwarding.
         */
        std::optional<bool> forward = std::nullopt;
        if (!has_subs) {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "No subscribers, pause publish connection handle: {0} using track alias: {1}",
                               connection_handle,
                               th.track_fullname_hash);

            if (bypass_resolve) {
                sub_track_handler->Pause();
            } else {
                forward = false;
                sub_track_handler->MarkPaused();
            }
        }

        state_.conn_state_metrics[connection_handle].published_tracks++;

        NotifySubscribeNamespaces(publish_attributes.track_full_name.name_space, connection_handle, false);

        return quicr::PublishResponse{ { .subscriber_priority = std::nullopt,
                                         .group_order = std::nullopt,
                                         .filter = {},
                                         .forward = forward,
                                         .subgroup_delivery_timeout = std::nullopt,
                                         .object_delivery_timeout = std::nullopt,
                                         .new_group_request_id = std::nullopt },
                                       sub_track_handler };
    }

    quicr::Reply<std::vector<quicr::TrackNamespace>, quicr::RequestErrorCode> ClientManager::SubscribeNamespaceReceived(
      const std::shared_ptr<quicr::Session>& session,
      const quicr::TrackNamespace& prefix_namespace,
      [[maybe_unused]] const quicr::SubscribeNamespaceAttributes& attributes)
    {

        const auto connection_handle = ConnectionHandle(session);
        auto th = quicr::TrackHash({ prefix_namespace, {} });

        auto [ns_it, _] = state_.subscribe_namespaces.try_emplace(prefix_namespace);
        const auto inserted = ns_it->second.insert(connection_handle).second;

        if (inserted) {
            SPDLOG_INFO(
              "Subscribe namespace received connection handle: {} for namespace_hash: {} prefix: {}, adding to state",
              connection_handle,
              th.track_namespace_hash,
              prefix_namespace.Str());
        } else {
            SPDLOG_INFO("Duplicate subscribe namespace connection handle: {} for namespace_hash: {} prefix: {}",
                        connection_handle,
                        th.track_namespace_hash,
                        prefix_namespace.Str());
            return quicr::Unexpected<quicr::Error<quicr::RequestErrorCode>>(
              quicr::RequestErrorCode::kDuplicateSubscription, "Duplicate subscribe namespace");
        }

        std::vector<quicr::TrackNamespace> matched_ns;

        // TODO: Fix O(prefix namespaces) matching
        for (const auto& [key, _] : state_.pub_namespace_active) {
            if (!config_.allow_self && key.second == connection_handle) {
                continue;
            }

            // Add matching announced namespaces to vector without duplicates
            const auto prefix_match = prefix_namespace.IsPrefixOf(key.first);
            if ((prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) &&
                (matched_ns.empty() || matched_ns.back() != key.first)) {
                matched_ns.push_back(key.first);
                notified_namespaces_.emplace(key.first, connection_handle);
            }
        }

        // TODO: Need to change this to use what peering is using to prefix match instead of O(n) over all publish
        //  subscribes
        for (const auto& [ta_conn, handler] : state_.pub_subscribes) {
            if (!handler || (!config_.allow_self && ta_conn.second == connection_handle)) {
                continue;
            }

            const auto& track_full_name = handler->GetFullTrackName();
            const auto prefix_match = prefix_namespace.IsPrefixOf(track_full_name.name_space);
            if (prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) {
                std::optional<quicr::messages::Location> largest_location = GetLargestAvailable(track_full_name);

                /*
                 * PublishTrack within the namespace handler determines if the track should be published or not
                 * based on filters
                 */
                const auto pub_handler = PublishTrackHandler::Create(
                  track_full_name,
                  quicr::TrackMode::kStream,
                  handler->GetSubscriberPriority(),
                  handler->GetSubscriberPriority(),
                  handler->GetDeliveryTimeout().value_or(std::chrono::milliseconds(kDefaultObjectTtl)).count(),
                  largest_location.value_or(quicr::messages::Location{ 0, 0 }),
                  *this);

                if (!pub_handler->GetTrackAlias().has_value()) {
                    auto pub_th = quicr::TrackHash(track_full_name);
                    pub_handler->SetTrackAlias(pub_th.track_fullname_hash);
                }

                if (notified_namespaces_.emplace(track_full_name.name_space, connection_handle).second) {
                    matched_ns.push_back(track_full_name.name_space);
                }

                SPDLOG_LOGGER_DEBUG(
                  LOGGER,
                  "Matched PUBLISH track for SUBSCRIBE_NAMESPACE: conn: {} track_alias: {} track_hash: {}",
                  connection_handle,
                  ta_conn.first,
                  quicr::TrackHash(track_full_name).track_fullname_hash);
            }
        }

        return matched_ns;
    }

    quicr::Reply<std::vector<quicr::TrackNamespace>, quicr::RequestErrorCode> ClientManager::SubscribeTracksReceived(
      const std::shared_ptr<quicr::Session>& session,
      const quicr::TrackNamespace& prefix_namespace,
      const quicr::SubscribeNamespaceAttributes& attributes)
    {
        const auto connection_handle = ConnectionHandle(session);
        auto th = quicr::TrackHash({ prefix_namespace, {} });

        auto [it, is_new] = state_.subscribe_tracks.try_emplace(prefix_namespace);

        auto handler = PublishNamespaceHandler::Create(prefix_namespace, GetTickService());
        PublishNamespace(connection_handle, handler);

        auto [pub_it, _] = it->second.emplace(connection_handle, handler);

        if (is_new) {
            SPDLOG_INFO(
              "Subscribe tracks received connection handle: {} for namespace_hash: {} prefix: {}, adding to state",
              connection_handle,
              th.track_namespace_hash,
              prefix_namespace.Str());
        }

        auto [ranks_it, __] = track_rankings_.try_emplace(th.track_namespace_hash, std::make_shared<TrackRanking>());

        if (const auto* tf = std::get_if<quicr::messages::TrackFilter>(&attributes.filter)) {
            if (ranks_it->second->GetInactiveAge() < tf->timeout) {
                ranks_it->second->SetInactiveAge(tf->timeout);
            }

            SPDLOG_INFO("Subscribe tracks track filter: property_type={} max_tracks={} timeout={}ms",
                        tf->property_type,
                        tf->max_tracks_selected,
                        tf->timeout);
            handler->SetMaxSelected(tf->max_tracks_selected);
            handler->SetInactiveAge(tf->timeout);
            handler->SetPropertyType(tf->property_type);
        } else {
            SPDLOG_INFO("Subscribe tracks has no track filter, using defaults");
        }

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
            if (!handler || (!config_.allow_self && ta_conn.second == connection_handle)) {
                continue;
            }

            const auto& track_full_name = handler->GetFullTrackName();
            const auto prefix_match = prefix_namespace.IsPrefixOf(track_full_name.name_space);
            if (prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) {
                std::optional<quicr::messages::Location> largest_location = GetLargestAvailable(track_full_name);

                /*
                 * PublishTrack within the namespace handler determines if the track should be published or not
                 * based on filters
                 */
                const auto pub_handler = PublishTrackHandler::Create(
                  track_full_name,
                  quicr::TrackMode::kStream,
                  handler->GetSubscriberPriority(),
                  handler->GetSubscriberPriority(),
                  handler->GetDeliveryTimeout().value_or(std::chrono::milliseconds(kDefaultObjectTtl)).count(),
                  largest_location.value_or(quicr::messages::Location{ 0, 0 }),
                  *this);

                if (!pub_handler->GetTrackAlias().has_value()) {
                    auto pub_th = quicr::TrackHash(track_full_name);
                    pub_handler->SetTrackAlias(pub_th.track_fullname_hash);
                }

                handler->AddSubscribeNamespace(pub_it->second);
                handler->SetTrackRanking(ranks_it->second);
                pub_it->second->PublishTrack(pub_handler);

                SPDLOG_LOGGER_DEBUG(
                  LOGGER,
                  "Matched PUBLISH track for SUBSCRIBE_TRACKS: conn: {} track_alias: {} track_hash: {}",
                  connection_handle,
                  ta_conn.first,
                  quicr::TrackHash(track_full_name).track_fullname_hash);
            }
        }

        return matched_ns;
    }

    void ClientManager::UnsubscribeNamespaceReceived(const std::shared_ptr<quicr::Session>& session,
                                                     const quicr::TrackNamespace& prefix_namespace)
    {
        const auto connection_handle = ConnectionHandle(session);
        DropSubscribeNamespace(connection_handle, prefix_namespace);

        auto it = state_.subscribe_tracks.find(prefix_namespace);
        if (it == state_.subscribe_tracks.end()) {
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
                state_.subscribe_tracks.erase(it);
            }
            return;
        }

        // Loop through publishes and remove subscribe namespace
        for (const auto& [ta_conn, handler] : state_.pub_subscribes) {
            if (!handler || (!config_.allow_self && ta_conn.second == connection_handle)) {
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
            state_.subscribe_tracks.erase(it);
            track_rankings_.erase(th.track_namespace_hash);
        }
    }

    quicr::Expected<void, quicr::Error<quicr::ErrorCode>> ClientManager::SetupReceived(
      const std::shared_ptr<quicr::Session>& session,
      const quicr::SetupAttributes& setup_attributes)
    {
        const auto connection_handle = ConnectionHandle(session);
        metrics_publisher_.SetConnectionEndpointId(connection_handle, setup_attributes.endpoint_id);

        SPDLOG_LOGGER_INFO(LOGGER, "Setup received from endpoint_id: {0}", setup_attributes.endpoint_id);

        return {};
    }

    void ClientManager::PublishDoneReceived(const std::shared_ptr<quicr::Session>& session, uint64_t request_id)
    {
        const auto connection_handle = ConnectionHandle(session);
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
            peer_manager_.Withdraw(ToPeeringTrack(s_it->second->GetFullTrackName()), moq::peering::Announcement::kTrack);

        const auto publisher_handler = s_it->second;

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

        auto& metrics_pub_tarcks = state_.conn_state_metrics[connection_handle].published_tracks;
        if (metrics_pub_tarcks > 0) {
            metrics_pub_tarcks--;
        }

        std::vector<std::pair<std::uint64_t, std::uint64_t>> unsub_list;

        if (!have_publishers) {

            // TODO: check if subscribe requests detatch or not

            if (!config_.detached_subs) {
                // Find subscribers that match this publisher and unsubscribe
                for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 });
                     it != state_.subscribes.end();
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

                lock.unlock();
                for (auto& [c_handle, req_id] : unsub_list) {
                    if (auto sub_session = GetSession(c_handle)) {
                        UnsubscribeReceived(sub_session, req_id);
                    }
                }

                lock.lock();
            }
        }

        const auto published_namespace = publisher_handler->GetFullTrackName().name_space;
        const auto pub_ta = publisher_handler->GetTrackAlias().value_or(0);

        // UnPublishTrack within publish namespace handler if matched
        for (auto& [tn, conns] : state_.subscribe_tracks) {
            const auto prefix_match = tn.IsPrefixOf(published_namespace);

            if (prefix_match == std::partial_ordering::less || prefix_match == std::partial_ordering::equivalent) {
                for (auto& [_, pub_ns_h] : conns) {
                    if (!config_.allow_self && pub_ns_h->GetConnectionId() == connection_handle) {
                        // Initially do not mirror
                        continue;
                    }

                    pub_ns_h->UnpublishTrack(pub_ta);
                }
            }
        }

        state_.pub_subscribes_by_req_id.erase(s_it);
        lock.unlock();

        if (!have_publishers) {
            publisher_handler->RemoveFromTrackRanking();
        }

        if (!NamespaceStillPublished(published_namespace)) {
            NotifySubscribeNamespaces(published_namespace, connection_handle, true);
        }
    }

    void ClientManager::UnsubscribeReceived(const std::shared_ptr<quicr::Session>& session, uint64_t request_id)
    {
        const auto connection_handle = ConnectionHandle(session);
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

        auto& metric_sub_count = state_.conn_state_metrics[connection_handle].subscribed_tracks;
        if (metric_sub_count > 0) {
            --metric_sub_count;
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

    void ClientManager::RemoveOrPausePublisherSubscribe(std::uint64_t track_fullname_hash)
    {
        // Do nothing if peering still has a subscriber
        if (peer_manager_.HasSubscribers(track_fullname_hash)) {
            return;
        }

        bool has_subs{ false };

        std::vector<std::pair<std::uint64_t, std::uint64_t>> remove_sub_pub;
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
            peer_manager_.Unsubscribe(track_fullname_hash);
        }
    }

    quicr::Reply<quicr::TrackStatusResponse, quicr::RequestErrorCode> ClientManager::TrackStatusReceived(
      const std::shared_ptr<quicr::Session>& session,
      const quicr::FullTrackName& track_full_name)
    {
        const auto connection_handle = ConnectionHandle(session);
        auto th = quicr::TrackHash(track_full_name);

        SPDLOG_LOGGER_INFO(LOGGER,
                           "Track status request connection handle: {} track alias: {}",
                           connection_handle,
                           th.track_fullname_hash);

        const auto largest = GetLargestAvailable(track_full_name);

        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {

            if (it->first.first != th.track_fullname_hash) {
                break;
            }

            if (it->first.second != connection_handle) {
                quicr::TrackStatusResponse response;
                response.largest_location = largest;
                return response;
            }
        }

        return quicr::Unexpected<quicr::Error<quicr::RequestErrorCode>>(quicr::RequestErrorCode::kDoesNotExist,
                                                                        "Track does not exist");
    }

    quicr::Reply<quicr::SubscribeResponse, quicr::RequestErrorCode> ClientManager::SubscribeReceived(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id,
      const quicr::FullTrackName& track_full_name,
      const quicr::SubscribeAttributes& attrs)
    {
        const auto connection_handle = ConnectionHandle(session);
        auto th = quicr::TrackHash(track_full_name);

        auto it = state_.subscribes.find({ th.track_fullname_hash, connection_handle });
        if (it != state_.subscribes.end()) {
            SPDLOG_LOGGER_INFO(
              LOGGER,
              "Duplicate subscribe connection handle: {0} request_id: {1} track alias: {2} priority: {3}",
              connection_handle,
              request_id,
              th.track_fullname_hash,
              attrs.priority);

            return quicr::Unexpected<quicr::Error<quicr::RequestErrorCode>>(
              quicr::RequestErrorCode::kDuplicateSubscription, "Duplicate subscribe");
        }

        SPDLOG_LOGGER_INFO(LOGGER,
                           "New subscribe connection handle: {} request_id: {} track alias: {} priority: {} delivery "
                           "timeout: {}ms tfn: {} ({})",
                           connection_handle,
                           request_id,
                           th.track_fullname_hash,
                           attrs.priority,
                           attrs.delivery_timeout.count(),
                           track_full_name.NamespaceStr(),
                           track_full_name.NameStr());

        auto largest = GetLargestAvailable(track_full_name);

        /*
         * libquicr sends the SUBSCRIBE_OK once this returns, so the subscriber cannot be forwarded anything
         * before it is acknowledged: the work below only arranges for forwarding, and the objects themselves
         * are not available until the publisher sends them.
         */
        ProcessSubscribe(connection_handle, request_id, th, track_full_name, attrs, largest);

        quicr::SubscribeResponse response;
        response.largest_location = largest;
        response.is_publisher_initiated = attrs.is_publisher_initiated;
        return response;
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

    quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> ClientManager::FetchReceived(
      std::uint64_t connection_handle,
      uint64_t request_id,
      const quicr::FullTrackName& track_full_name,
      uint8_t priority,
      std::optional<quicr::messages::GroupOrder> group_order,
      quicr::messages::Location start,
      quicr::messages::FetchEndLocation end)
    {
        using Reply = quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode>;
        using Failure = quicr::Unexpected<quicr::Error<quicr::FetchErrorCode>>;

        const auto th = quicr::TrackHash(track_full_name);
        const auto resolved_group_order = group_order.value_or(quicr::messages::GroupOrder::kAscending);

        const auto largest_location = GetLargestAvailable(track_full_name);

        const auto cache_entry_it = cache_.find(th.track_fullname_hash);
        auto cache_entries = cache_entry_it != cache_.end() ? cache_entry_it->second.Get(start.group, end.group)
                                                            : std::vector<std::shared_ptr<std::set<CacheObject>>>{};

        // Why the cache cannot answer the fetch, which is the error reported if the publisher cannot either
        std::optional<quicr::FetchErrorCode> cache_miss = std::nullopt;
        if (!largest_location.has_value()) {
            // TODO: This changes to send an empty object instead of REQUEST_ERROR
            cache_miss = quicr::FetchErrorCode::kNoObjects;
        } else if (start.group > end.group || largest_location->group < start.group) {
            cache_miss = quicr::FetchErrorCode::kInvalidRange;
        } else if (cache_entries.empty()) {
            cache_miss = quicr::FetchErrorCode::kNoObjects;
        }

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

        // TODO: Adjust the TTL to allow more time for transmission
        auto pub_fetch_h = quicr::PublishFetchHandler::Create(
          track_full_name, priority, request_id, resolved_group_order, config_.object_ttl_);
        BindFetchTrack(connection_handle, pub_fetch_h);

        stop_fetch_.try_emplace({ connection_handle, request_id }, false);

        if (cache_miss.has_value()) {
            /*
             * Nothing usable is cached, so the publisher is asked for the range instead. Whether the fetch can
             * be answered at all is only known once the publisher responds, so the reply is deferred to keep
             * that wait off the session's thread.
             */
            return Reply::Defer([=, this](Reply::CompletionType&& complete) {
                auto track_handler = FetchTrackHandler::Create(pub_fetch_h,
                                                               track_full_name,
                                                               priority,
                                                               { .group = start.group, .object = start.object },
                                                               { .group = end.group, .object = end.object },
                                                               resolved_group_order);

                // Find the publisher connection handle to send the fetch request
                // TODO: Add peering support
                std::uint64_t pub_connection_handle = 0;
                {
                    std::lock_guard _(state_.state_mutex);

                    const auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
                    if (it != state_.pub_subscribes.end() && it->first.first == th.track_fullname_hash) {
                        pub_connection_handle = it->first.second; // TODO: Support multiple publishers
                    }
                }

                if (pub_connection_handle == 0) {
                    UnbindFetchTrack(connection_handle, pub_fetch_h);
                    complete(Failure(*cache_miss, "Cannot process fetch"));
                    return;
                }

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

                /*
                 * The publisher keeps forwarding through pub_fetch_h after this reply is answered, so its
                 * teardown waits on the upstream fetch from a thread of its own.
                 */
                std::thread([=, this] {
                    defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

                    while (track_handler->GetStatus() == quicr::FetchTrackHandler::Status::kOk) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                    CancelFetchTrack(pub_connection_handle, track_handler);
                }).detach();

                switch (track_handler->GetStatus()) {
                    case quicr::FetchTrackHandler::Status::kDoneByFin:
                        [[fallthrough]];
                    case quicr::FetchTrackHandler::Status::kOk:
                        break;

                    case quicr::FetchTrackHandler::Status::kError:
                        complete(Failure(quicr::FetchErrorCode::kNoObjects, "Cannot process fetch"));
                        return;

                    default:
                        complete(Failure(quicr::FetchErrorCode::kInternalError, "Cannot process fetch"));
                        return;
                }

                const auto upstream_largest = track_handler->GetLatestLocation();
                if (!upstream_largest.has_value()) {
                    complete(Failure(quicr::FetchErrorCode::kNoObjects, "Cannot process fetch"));
                    return;
                }

                complete(quicr::FetchResponse{ upstream_largest, resolved_group_order });
            });
        }

        SPDLOG_LOGGER_DEBUG(
          LOGGER, "Fetch received conn_id: {} request_id: {}, using cache", connection_handle, request_id);

        /*
         * The cached objects are sent from their own thread so that the reply below can be answered right away,
         * which is what sends the FETCH_OK. The objects go out on a stream of their own, so they are not
         * ordered against that FETCH_OK either way.
         */
        std::thread retrieve_cache_thread([=, cache_entries = std::move(cache_entries), this] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

            const auto publish_group = [&](const std::set<CacheObject>& group) {
                for (const auto& object : group) {
                    if (stop_fetch_[{ connection_handle, request_id }]) {
                        stop_fetch_.erase({ connection_handle, request_id });
                        return false;
                    }

                    // Start at start object id
                    if (start.group == object.headers.group_id && start.object > object.headers.object_id) {
                        continue;
                    }

                    // Skip past end object, unless end object is zero
                    if (end.object.has_value() && object.headers.group_id == end.group &&
                        object.headers.object_id > *end.object) {
                        continue;
                    }

                    SPDLOG_LOGGER_TRACE(
                      LOGGER, "Fetching group: {} object: {}", object.headers.group_id, object.headers.object_id);

                    try {
                        pub_fetch_h->PublishObject(object.headers, object.data);
                    } catch (const std::exception& e) {
                        SPDLOG_LOGGER_ERROR(LOGGER, "Caught exception sending fetch object: {}", e.what());
                    }
                }

                return true;
            };

            // Fetch objects are delta encoded against the previously sent object, so groups have to be
            // sent in the requested group order. Object IDs remain ascending within a group.
            if (resolved_group_order == quicr::messages::GroupOrder::kDescending) {
                for (auto entry_it = cache_entries.rbegin(); entry_it != cache_entries.rend(); ++entry_it) {
                    if (!publish_group(**entry_it)) {
                        return;
                    }
                }
            } else {
                for (const auto& entry : cache_entries) {
                    if (!publish_group(*entry)) {
                        return;
                    }
                }
            }
        });

        retrieve_cache_thread.detach();

        return quicr::FetchResponse{ largest_location, resolved_group_order };
    }

    quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> ClientManager::StandaloneFetchReceived(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id,
      const quicr::FullTrackName& track_full_name,
      const quicr::StandaloneFetchAttributes& attributes)
    {
        const auto connection_handle = ConnectionHandle(session);
        return FetchReceived(connection_handle,
                             request_id,
                             track_full_name,
                             attributes.priority,
                             attributes.group_order,
                             attributes.start_location,
                             attributes.end_location);
    }

    quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> ClientManager::JoiningFetchReceived(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id,
      const quicr::FullTrackName& track_full_name,
      const quicr::JoiningFetchAttributes& attributes)
    {
        const auto connection_handle = ConnectionHandle(session);
        const auto largest_location = GetLargestAvailable(track_full_name);

        // No largest location is an error.
        if (!largest_location.has_value()) {
            return quicr::Unexpected<quicr::Error<quicr::FetchErrorCode>>(quicr::FetchErrorCode::kInvalidRange,
                                                                          "No objects available for joining fetch");
        }

        uint64_t joining_start = 0;
        if (attributes.relative) {
            if (largest_location->group > attributes.joining_start)
                joining_start = largest_location->group - attributes.joining_start;
        } else {
            joining_start = attributes.joining_start;
        }

        return FetchReceived(connection_handle,
                             request_id,
                             track_full_name,
                             attributes.priority,
                             attributes.group_order,
                             { joining_start, 0 },
                             { largest_location->group, std::nullopt });
    }

    quicr::Reply<void, quicr::FetchErrorCode> ClientManager::FetchCancelReceived(
      const std::shared_ptr<quicr::Session>& session,
      uint64_t request_id)
    {
        const auto connection_handle = ConnectionHandle(session);
        SPDLOG_INFO("Canceling fetch for connection_handle: {} request_id: {}", connection_handle, request_id);

        if (stop_fetch_.count({ connection_handle, request_id }) == 0)
            stop_fetch_[{ connection_handle, request_id }] = true;

        return {};
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

    void ClientManager::OnPeerSubscribe(const moq::peering::TrackName& name,
                                        std::uint64_t,
                                        const moq::peering::SubscribeParams& params)
    {
        const auto track_full_name = ToQuicrTrack(name);
        const auto th = quicr::TrackHash(track_full_name);

        // Same lock PublishReceived holds until the handler's session is bound and published.
        std::lock_guard _(state_.state_mutex);

        bool matches = false;
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            if (it->first.first != th.track_fullname_hash) {
                break;
            }
            matches = true;
        }

        if (!matches) {
            for (auto& [key, track_aliases] : state_.pub_namespace_active) {
                if (!key.first.HasSamePrefix(track_full_name.name_space)) {
                    continue;
                }
                matches = true;
                break;
            }
        }

        if (!matches) {
            return;
        }

        ProcessSubscribe(0, 0, th, track_full_name, ToQuicrParams(params), std::nullopt);
    }

    void ClientManager::OnPeerUnsubscribe(std::uint64_t track_hash, bool subscribers_remain)
    {
        std::lock_guard _(state_.state_mutex);

        if (!subscribers_remain) {
            PeerUnsubscribeTrack(track_hash);
        }
        RemoveOrPausePublisherSubscribe(track_hash);
    }

    void ClientManager::OnPeerNamespace(const std::vector<std::vector<uint8_t>>& name_space, bool withdrawn)
    {
        std::lock_guard _(state_.state_mutex);

        const auto track_namespace = ToQuicrNamespace(name_space);
        const auto th = quicr::TrackHash({ track_namespace, {} });
        if (!withdrawn) {
            PublishNamespaceReceived(nullptr, track_namespace, { .request_id = th.track_namespace_hash });
        } else {
            PublishNamespaceDoneReceived(nullptr, th.track_namespace_hash);
        }
    }

    void ClientManager::OnPeerTrack(const moq::peering::TrackName& name, bool withdrawn)
    {
        if (!withdrawn) {
            const auto track_full_name = ToQuicrTrack(name);
            quicr::PublishAttributes attrs{ .track_full_name = track_full_name,
                                            .track_alias = quicr::TrackHash(track_full_name).track_fullname_hash,
                                            .auth_tokens = {},
                                            .expires = std::nullopt,
                                            .largest_object = std::nullopt,
                                            .forward = true,
                                            .default_publisher_group_order = quicr::messages::GroupOrder::kAscending,
                                            .dynamic_groups = true,
                                            .publisher_priority = 64,
                                            .max_cache_duration = std::nullopt,
                                            .delivery_timeout = kDefaultObjectTtl,
                                            .track_properties = {} };
            PublishReceived(nullptr, 0, attrs, {});
        } else {
            PublishDoneReceived(nullptr, 0);
        }
    }

    void ClientManager::OnPeerData(std::uint64_t track_hash,
                                   bool is_new_stream,
                                   std::optional<uint64_t> stream_id,
                                   std::shared_ptr<const std::vector<uint8_t>> data)
    {
        PeerDataReceived(track_hash, is_new_stream, stream_id, std::move(data));
    }

    void ClientManager::OnPeerStreamClosed(std::uint64_t track_hash, std::uint64_t stream_id, bool reset)
    {
        PeerStreamClosed(track_hash, stream_id, reset);
    }

    void ClientManager::ProcessSubscribe(std::uint64_t connection_handle,
                                         uint64_t request_id,
                                         const quicr::TrackHash& th,
                                         const quicr::FullTrackName& track_full_name,
                                         const quicr::SubscribeAttributes& attrs,
                                         std::optional<quicr::messages::Location> largest)
    {

        auto start_location = attrs.start_location;
        bool sub_from_peer{ false };

        if (largest.has_value()) {
            SPDLOG_LOGGER_INFO(LOGGER, "Subscribe largest group: {} object: {}", largest->group, largest->object);
        }

        if (connection_handle == 0 && request_id == 0) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER,
              "Processing peer subscribe track alias: {} priority: {} new_group_request: {} tfn: {} / {}",
              th.track_fullname_hash,
              attrs.priority,
              attrs.new_group_request_id ? std::to_string(*attrs.new_group_request_id) : std::string("none"),
              track_full_name.NamespaceStr(),
              track_full_name.NameStr());

            sub_from_peer = true;
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
                               attrs.new_group_request_id ? std::to_string(*attrs.new_group_request_id)
                                                          : std::string("none"),
                               start_location.group,
                               start_location.object);

            state_.conn_state_metrics[connection_handle].subscribed_tracks++;

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

            peer_manager_.Subscribe(ToPeeringTrack(track_full_name), ToPeeringParams(attrs));
        }

        // Resume publisher initiated subscribes
        for (auto it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             it != state_.pub_subscribes.end();
             ++it) {
            if (it->first.first != th.track_fullname_hash) {
                break;
            }

            it->second->AddSubscriber(
              connection_handle, request_id, attrs.priority, attrs.delivery_timeout, attrs.start_location);

            DampenOrUpdateTrackSubscription(it->second, attrs.new_group_request_id.has_value() || sub_from_peer);
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

            } else {
                auto pub_handler_it = state_.pub_subscribes.find({ th.track_fullname_hash, key.second });
                if (pub_handler_it != state_.pub_subscribes.end()) {
                    DampenOrUpdateTrackSubscription(pub_handler_it->second, attrs.new_group_request_id.has_value());
                }
            }
        }
    }

    void ClientManager::PeerUnsubscribeTrack(std::uint64_t track_full_name_hash)
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

    bool ClientManager::PublishLocalObject(std::uint64_t track_fullname_hash,
                                           const quicr::ObjectHeaders& object_headers,
                                           quicr::BytesSpan data)
    {
        const auto it = state_.pub_subscribes.find({ track_fullname_hash, 0 });
        if (it == state_.pub_subscribes.end() || !it->second) {
            return false;
        }

        it->second->ObjectReceived(object_headers, data);
        return true;
    }

    void ClientManager::PeerDataReceived(std::uint64_t track_full_name_hash,
                                         bool is_new_stream,
                                         std::optional<uint64_t> stream_id,
                                         std::shared_ptr<const std::vector<uint8_t>> data)
    {
        const auto it = state_.pub_subscribes.find({ track_full_name_hash, 0 });
        if (it == state_.pub_subscribes.end()) {
            return;
        }

        if (stream_id.has_value()) {
            it->second->PeerStreamDataReceived(*stream_id, std::move(data), is_new_stream);
        } else {
            it->second->DgramDataRecv(std::move(data));
        }
    }

    void ClientManager::PeerStreamClosed(std::uint64_t track_full_name_hash, uint64_t stream_id, bool reset)
    {
        const auto it = state_.pub_subscribes.find({ track_full_name_hash, 0 });
        if (it == state_.pub_subscribes.end()) {
            return;
        }

        it->second->PeerStreamClosedReceived(stream_id, reset);
    }

    void ClientManager::MetricsSampled(const std::shared_ptr<quicr::Session>& session,
                                       const quicr::ConnectionMetrics& metrics)
    {
        const auto connection_handle = ConnectionHandle(session);
        const auto publish_track_count = state_.PublishTrackCount(connection_handle);
        const auto subscribe_track_count = state_.SubscribeTrackCount(connection_handle);
        metrics_publisher_.QueueConnectionMetrics(
          connection_handle, publish_track_count, subscribe_track_count, metrics);
    }
}
