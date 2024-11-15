// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#include "peer_manager.h"
#include "client_manager.h"
#include "state.h"
#include <chrono>
#include <sstream>

namespace laps::peering {

    // ------------------------------------------------------------
    // Methods used by peer session to feedback info and actions
    // ------------------------------------------------------------

    void PeerManager::NodeReceived(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Node peer_session_id: {} received id: {} contact: {} should delete = {}",
                           peer_session_id,
                           node_info.id,
                           node_info.contact,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);

        if (not withdraw) {
            if (info_base_->AddNode(peer_session, node_info)) {
                // Add peer to path before advertising node
                auto adv_node_info = node_info;
                adv_node_info.path.push_back({ adv_node_info.id, peer_session->metrics_.srtt_us });
                PropagateNodeInfo(adv_node_info);
            }
        } else {
            info_base_->RemoveNode(peer_session_id, node_info.id);

            auto adv_node_info = node_info;
            adv_node_info.path.push_back({ peer_session->node_info_.id, peer_session->metrics_.srtt_us });
            PropagateNodeInfo(adv_node_info, true);
        }
    } catch (const std::exception&) {
        // ignore
    }

    void PeerManager::SubscribeInfoReceived(PeerSessionId peer_session_id,
                                            const SubscribeInfo& subscribe_info,
                                            bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Subscribe info received peer_session_id: {} fullname: {} withdraw: {}",
                           peer_session_id,
                           subscribe_info.track_hash.track_fullname_hash,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);
        if (not withdraw && !info_base_->AddSubscribe(subscribe_info)) {
            // Don't send to other peers if we have seen this before (loop)
            return;
        }

        for (const auto& sess : client_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendSubscribeInfo(subscribe_info, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendSubscribeInfo(subscribe_info, withdraw);
        }

        /*
         * On subscribe, check if there are client announcements that match it. If so,
         * find best peer session and update peer session with subscribe node id
         * and send notification to client manager that there is a subscribe.
         */
        if (not withdraw) {
            auto it = state_.announce_active.lower_bound({ subscribe_info.track_hash.track_namespace_hash, 0 });
            if (it != state_.announce_active.end()) {
                SPDLOG_LOGGER_INFO(
                  LOGGER, "Announce matched subscribe fullname: {}", subscribe_info.track_hash.track_fullname_hash);

                if (auto cm = client_manager_.lock()) {

                    quicr::SubscribeAttributes s_attrs;
                    s_attrs.priority = 10;

                    quicr::messages::MoqSubscribe sub;
                    subscribe_info.subscribe_data >> sub;

                    SPDLOG_LOGGER_INFO(LOGGER, "Subscribe to client manager track alias: {}", sub.track_alias);

                    cm->ProcessSubscribe(
                      0, 0, subscribe_info.track_hash, { sub.track_namespace, sub.track_name }, s_attrs);
                }
            }

            auto bp_it = info_base_->nodes_best_.find(subscribe_info.source_node_id);
            if (bp_it != info_base_->nodes_best_.end()) {
                const auto& peer_session = bp_it->second.lock();
                SPDLOG_LOGGER_DEBUG(
                  LOGGER,
                  "Best peer session for subscribe fullname: {} source_node: {} is via peer_session_id: {}",
                  subscribe_info.track_hash.track_fullname_hash,
                  subscribe_info.source_node_id,
                  peer_session->GetSessionId());

                if (auto [sns_id, is_new] = peer_session->AddSubscribeSourceNode(
                      subscribe_info.track_hash.track_fullname_hash, subscribe_info.source_node_id);
                    is_new) {
                    SPDLOG_LOGGER_INFO(LOGGER,
                                       "New source added to peer session for subscribe fullname: {} source_node: {} is "
                                       "via peer_session_id: {} sns_id: {}",
                                       subscribe_info.track_hash.track_fullname_hash,
                                       subscribe_info.source_node_id,
                                       peer_session->GetSessionId(),
                                       sns_id);

                    if (auto [_, is_new] = info_base_->client_fib_.try_emplace(
                          { subscribe_info.track_hash.track_fullname_hash, peer_session_id },
                          InfoBase::FibEntry{ sns_id, bp_it->second });
                        is_new) {
                        SPDLOG_LOGGER_INFO(LOGGER,
                                           "New subscribe fullname: {}, sending subscribe to client manager",
                                           subscribe_info.track_hash.track_fullname_hash);
                        /*
                                                    if (auto cm = client_manager_.lock()) {

                                                        quicr::SubscribeAttributes s_attrs;
                                                        s_attrs.priority = 10;

                                                        quicr::messages::MoqSubscribe sub;
                                                        subscribe_info.subscribe_data >> sub;

                                                        SPDLOG_LOGGER_INFO(
                                                          LOGGER, "Subscribe to client manager track alias: {}",
                           sub.track_alias);

                                                        cm->ProcessSubscribe(0,
                                                                             0,
                                                                             subscribe_info.track_hash,
                                                                             { sub.track_namespace, sub.track_name
                           }, s_attrs);
                                                    }
                                                    */
                    }
                }
            }
        } else {
            auto bp_it = info_base_->nodes_best_.find(subscribe_info.source_node_id);
            if (bp_it != info_base_->nodes_best_.end()) {
                const auto& peer_session = bp_it->second.lock();
                if (const auto [_, sns_removed] = peer_session->RemoveSubscribeSourceNode(
                      subscribe_info.track_hash.track_fullname_hash, subscribe_info.source_node_id);
                    sns_removed) {
                    SPDLOG_LOGGER_INFO(LOGGER,
                                       "No subscribe nodes left via peer session {}, removed subscribe fullname: {}",
                                       peer_session->GetSessionId(),
                                       subscribe_info.track_hash.track_fullname_hash);

                    info_base_->client_fib_.erase({ subscribe_info.track_hash.track_fullname_hash, peer_session_id });

                    bool has_subscribe_peers{ false };
                    for (auto it =
                           info_base_->client_fib_.lower_bound({ subscribe_info.track_hash.track_fullname_hash, 0 });
                         it != info_base_->client_fib_.end();
                         ++it) {
                        auto& [key, _] = *it;

                        if (key.first != subscribe_info.track_hash.track_fullname_hash)
                            break;

                        has_subscribe_peers = true;
                        break;
                    }

                    if (not has_subscribe_peers) {
                        SPDLOG_LOGGER_INFO(LOGGER,
                                           "No peers left for subscribe fullname: {}, removing client subscribe state",
                                           subscribe_info.track_hash.track_fullname_hash);

                        if (auto cm = client_manager_.lock()) {
                            cm->RemovePublisherSubscribe(subscribe_info.track_hash);
                        }
                    }
                }
            }
        }

    } catch (const std::exception&) {
        // ignore
    }

    void PeerManager::AnnounceInfoReceived(PeerSessionId peer_session_id,
                                           const AnnounceInfo& announce_info,
                                           bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Announce info received peer_session_id: {} hash: {} withdraw: {}",
                           peer_session_id,
                           announce_info.full_name.full_name_hash,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);
        if (not withdraw && !info_base_->AddAnnounce(announce_info)) {
            // Don't send to other peers if we have seen this before (loop)
            return;
        }

        for (const auto& sess : client_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendAnnounceInfo(announce_info, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendAnnounceInfo(announce_info, withdraw);
        }

    } catch (const std::exception&) {
        // ignore
    }

    void PeerManager::SessionChanged(PeerSessionId peer_session_id,
                                     PeerSession::StatusValue status,
                                     [[maybe_unused]] const NodeInfo& remote_node_info)
    {
        switch (status) {
            case PeerSession::StatusValue::kConnected: {
                SPDLOG_LOGGER_INFO(LOGGER, "Peer session connected peer_session_id: ", peer_session_id);

                std::lock_guard _(info_base_->mutex_);

                for (auto& [node_id, peer_sess] : info_base_->nodes_best_) {
                    auto& node_item = info_base_->nodes_.at({ node_id, peer_sess.lock()->GetSessionId() });

                    auto adv_node_info = node_item.node_info;
                    adv_node_info.path.push_back({ adv_node_info.id, node_item.peer_session.lock()->metrics_.srtt_us });
                    PropagateNodeInfo(peer_session_id, node_item.node_info);
                }

                break;
            }
            case PeerSession::StatusValue::kConnecting:
                break;
            default:
                SPDLOG_LOGGER_INFO(LOGGER, "Peer session not connected peer_session_id: {}", peer_session_id);

                PropagateNodeInfo(remote_node_info, true);

                if (!stop_)
                    info_base_->PurgePeerSessionInfo(peer_session_id);
                break;
        }
    }

    void PeerManager::ClientSubscribe(const quicr::FullTrackName& track_full_name,
                                      const quicr::SubscribeAttributes&,
                                      Span<const uint8_t> subscribe_data,
                                      bool withdraw)
    {
        quicr::TrackHash th(track_full_name);

        SubscribeInfo si;

        si.track_hash = th;
        si.subscribe_data.assign(subscribe_data.begin(), subscribe_data.end());

        for (const auto& sess : client_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending subscribe fullname: {} peer_session_id: {}",
                                si.track_hash.track_fullname_hash,
                                sess.first);
            si.source_node_id = sess.second->node_info_.id;
            sess.second->SendSubscribeInfo(si, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending subscribe fullname: {} peer_session_id: {}",
                                si.track_hash.track_fullname_hash,
                                sess.first);
            si.source_node_id = sess.second->node_info_.id;
            sess.second->SendSubscribeInfo(si, withdraw);
        }
    }

    void PeerManager::ClientAnnounce(const quicr::FullTrackName& track_full_name,
                                     const quicr::PublishAnnounceAttributes&,
                                     bool withdraw)
    {
        quicr::TrackHash th(track_full_name);

        AnnounceInfo ai;

        ai.full_name.full_name_hash = th.track_fullname_hash;
        ai.full_name.namespace_tuples.reserve(track_full_name.name_space.GetHashes().size());

        for (const auto ns_item : track_full_name.name_space.GetHashes()) {
            ai.full_name.namespace_tuples.push_back(ns_item);
        }

        ai.full_name.name_hash = th.track_name_hash;

        for (const auto& sess : client_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Sending announce hash: {} peer_session_id: {}", th.track_fullname_hash, sess.first);
            ai.source_node_id = sess.second->node_info_.id;
            sess.second->SendAnnounceInfo(ai, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Sending announce hash: {} peer_session_id: {}", th.track_fullname_hash, sess.first);
            ai.source_node_id = sess.second->node_info_.id;
            sess.second->SendAnnounceInfo(ai, withdraw);
        }
    }

    // -------------------------------------------------------------
    // Peer Manager
    // -------------------------------------------------------------

    PeerManager::PeerManager(const Config& cfg, State& state, std::shared_ptr<InfoBase> info_base)
      : info_base_(info_base)
      , tick_service_(std::make_shared<quicr::ThreadedTickService>())
      , config_(cfg)
      , state_(state)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "Peering manager Node ID: {} listening port: {}", config_.relay_id_, config_.peering.listening_port);

        check_thr_ = std::thread(&PeerManager::CheckThread, this, cfg.peering.check_interval_ms);

        node_info_.contact = config_.relay_id_;
        node_info_.id = std::hash<std::string>{}(config_.relay_id_);
        node_info_.type = config_.node_type;

        quicr::TransportRemote server{ "0.0.0.0", config_.peering.listening_port, quicr::TransportProtocol::kQuic };

        quicr::TransportConfig tconfig;
        tconfig.debug = cfg.debug;
        tconfig.tls_cert_filename = config_.tls_cert_filename_;
        tconfig.tls_key_filename = config_.tls_key_filename_;
        tconfig.time_queue_init_queue_size = config_.peering.init_queue_size;
        tconfig.time_queue_max_duration = config_.peering.max_ttl_expiry_ms;

        server_transport_ =
          quicr::ITransport::MakeServerTransport(std::move(server), std::move(tconfig), *this, tick_service_, LOGGER);
        server_transport_->Start();

        while (server_transport_->Status() == quicr::TransportStatus::kConnecting) {
            SPDLOG_LOGGER_INFO(LOGGER, "Waiting for server to be ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (const auto& [host, port] : cfg.peering.peers) {
            quicr::TransportRemote remote{ host, port, quicr::TransportProtocol::kQuic };
            CreatePeerSession(std::move(remote));
        }
    }

    PeerManager::~PeerManager()
    {
        // Stop threads
        stop_ = true;

        SPDLOG_LOGGER_INFO(LOGGER, "Closing peer manager threads");

        client_peer_sessions_.clear();
        server_peer_sessions_.clear();

        if (check_thr_.joinable())
            check_thr_.join();

        SPDLOG_LOGGER_INFO(LOGGER, "Closed peer manager stopped");
    }

    void PeerManager::PropagateNodeInfo(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw)
    {
        std::lock_guard _(mutex_);

        auto server_it = server_peer_sessions_.find(peer_session_id);
        if (server_it != server_peer_sessions_.end()) {
            if (server_it->second->remote_node_info_.id != node_info.id)
                server_it->second->SendNodeInfo(node_info, withdraw);
            return;
        }

        auto client_it = client_peer_sessions_.find(peer_session_id);
        if (client_it != client_peer_sessions_.end()) {
            if (client_it->second->remote_node_info_.id != node_info.id)
                client_it->second->SendNodeInfo(node_info, withdraw);
            return;
        }
    }

    void PeerManager::PropagateNodeInfo(const NodeInfo& node_info, bool withdraw)
    {
        auto skip_node = [](const NodeInfo& node_a, const NodeInfo& node_b) -> bool {
            if (node_a.id == node_b.id)
                return true;

            for (auto& npi : node_a.path) {
                if (npi.id == node_b.id) {
                    return true;
                }
            }

            return false;
        };

        std::lock_guard _(mutex_);

        // Advertise node info to peer
        for (auto& [_, peer_session] : client_peer_sessions_) {
            if (skip_node(node_info, peer_session->remote_node_info_))
                continue;

            peer_session->SendNodeInfo(node_info, withdraw);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} contact: {} withdraw: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact,
                                withdraw);
        }

        for (auto& [_, peer_session] : server_peer_sessions_) {
            if (skip_node(node_info, peer_session->remote_node_info_))
                continue;

            peer_session->SendNodeInfo(node_info, withdraw);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} --> contact: {} withdraw: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact,
                                withdraw);
        }
    }

    inline std::shared_ptr<PeerSession> PeerManager::GetPeerSession(const PeerSessionId peer_session_id)
    {
        auto s_peer_it = server_peer_sessions_.find(peer_session_id);
        if (s_peer_it != server_peer_sessions_.end()) {
            return s_peer_it->second;
        }

        auto c_peer_it = client_peer_sessions_.find(peer_session_id);
        if (c_peer_it != client_peer_sessions_.end()) {
            return c_peer_it->second;
        }

        throw std::invalid_argument("Peer session id does not exist");
    }

    void PeerManager::CreatePeerSession(const quicr::TransportRemote& peer_config)
    {
        auto peer_sess = std::make_shared<PeerSession>(false, 0, config_, node_info_, peer_config, *this);
        peer_sess->Connect();

        client_peer_sessions_.try_emplace(peer_sess->GetSessionId(), std::move(peer_sess));
    }

    void PeerManager::CheckThread(int interval_ms)
    {
        SPDLOG_LOGGER_INFO(LOGGER, "Running peer manager outbound peer connection thread");

        if (interval_ms < 2000)
            interval_ms = 2000;

        auto start = std::chrono::system_clock::now();

        while (not stop_) {
            auto check = std::chrono::system_clock::now();

            // Run check only after interval ms
            if (std::chrono::duration_cast<std::chrono::milliseconds>(check - start).count() >= interval_ms) {
                start = std::chrono::system_clock::now();

                for (auto& [_, sess] : client_peer_sessions_) {
                    if (sess->Status() == PeerSession::StatusValue::kDisconnected) {
                        SPDLOG_LOGGER_INFO(LOGGER, "Peer session {} disconnected, reconnecting", sess->GetSessionId());

                        sess->Connect();
                    }
                }
            }

            // Sleep shorter so that the loop can be stopped within this time instead of a larger interval
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms / 2));
        }
    }

    /*
     * Delegate Implementations
     */
    void PeerManager::OnConnectionStatus(const quicr::TransportConnId& conn_id, const quicr::TransportStatus status)
    {
        auto peer_it = server_peer_sessions_.find(conn_id);
        if (peer_it == server_peer_sessions_.end()) {
            return;
        }

        PeerSession::StatusValue sess_status = PeerSession::StatusValue::kConnected;
        switch (status) {
            case quicr::TransportStatus::kReady: {
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is connected", conn_id);
                break;
            }
            case quicr::TransportStatus::kConnecting:
                break;

            case quicr::TransportStatus::kDisconnected: {
                sess_status = PeerSession::StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is disconnected", conn_id);
                break;
            }

            case quicr::TransportStatus::kRemoteRequestClose:
                sess_status = PeerSession::StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} remote disconnected", conn_id);
                break;

            case quicr::TransportStatus::kShutdown:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;

            case quicr::TransportStatus::kIdleTimeout:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} idle timeout", conn_id);
                break;

            case quicr::TransportStatus::kShuttingDown:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;
        }

        SessionChanged(peer_it->second->GetSessionId(), sess_status, peer_it->second->remote_node_info_);

        server_peer_sessions_.erase(peer_it);
    }

    void PeerManager::OnNewConnection(const quicr::TransportConnId& conn_id, const quicr::TransportRemote& remote)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);

        if (peer_iter == server_peer_sessions_.end()) {
            SPDLOG_LOGGER_INFO(LOGGER, "New server accepted peer, conn_id: {0}", conn_id);

            quicr::TransportRemote peer = remote;
            auto [iter, inserted] = server_peer_sessions_.try_emplace(
              conn_id, std::make_shared<PeerSession>(true, conn_id, config_, node_info_, std::move(peer), *this));

            peer_iter = iter;

            auto& peer_sess = peer_iter->second;

            peer_sess->SetTransport(server_transport_);
            peer_sess->Connect();
        }
    }

    void PeerManager::OnRecvStream(const quicr::TransportConnId& conn_id,
                                   uint64_t stream_id,
                                   std::optional<quicr::DataContextId> data_ctx_id,
                                   const bool is_bidir)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);
        if (peer_iter != server_peer_sessions_.end()) {
            peer_iter->second->OnRecvStream(conn_id, stream_id, data_ctx_id, is_bidir);
        }
    }

    void PeerManager::OnRecvDgram(const quicr::TransportConnId& conn_id,
                                  std::optional<quicr::DataContextId> data_ctx_id)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);
        if (peer_iter != server_peer_sessions_.end()) {
            peer_iter->second->OnRecvDgram(conn_id, data_ctx_id);
        }
    }

} // namespace laps