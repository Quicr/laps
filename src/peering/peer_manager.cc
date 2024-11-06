// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#include "peer_manager.h"
#include "state.h"
#include <chrono>
#include <sstream>

namespace laps::peering {

    // ------------------------------------------------------------
    // Methods used by peer session to feedback info and actions
    // ------------------------------------------------------------

    void PeerManager::NodeReceived(PeerSessionId peer_session_id, const NodeInfo& node_info, bool remove)
    {
        SPDLOG_LOGGER_INFO(config_.logger_,
                           "Node received id: {} contact: {} should delete = {}",
                           node_info.id,
                           node_info.contact,
                           remove);

        try {
            auto peer_session = GetPeerSession(peer_session_id);

            if (info_base_->AddNode(peer_session, node_info)) {
                // Add peer to path before advertising node
                auto adv_node_info = node_info;
                adv_node_info.path.push_back({ adv_node_info.id, peer_session->metrics_.srtt_us });
                PropagateNodeInfo(adv_node_info);
            }
        } catch (const std::exception&) {
            // ignore
        }
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

                if (!stop_)
                    info_base_->RemoveNodes(peer_session_id);
                break;
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

    void PeerManager::PropagateNodeInfo(PeerSessionId peer_session_id, const NodeInfo& node_info)
    {
        std::lock_guard _(mutex_);

        auto server_it = server_peer_sessions_.find(peer_session_id);
        if (server_it != server_peer_sessions_.end()) {
            if (server_it->second->remote_node_info_.id != node_info.id)
                server_it->second->SendNodeInfo(node_info);
            return;
        }


        auto client_it = client_peer_sessions_.find(peer_session_id);
        if (client_it != client_peer_sessions_.end()) {
            if (client_it->second->remote_node_info_.id != node_info.id)
                client_it->second->SendNodeInfo(node_info);
            return;
        }
    }

    void PeerManager::PropagateNodeInfo(const NodeInfo& node_info)
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

            peer_session->SendNodeInfo(node_info);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} contact: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact);
        }

        for (auto& [_, peer_session] : server_peer_sessions_) {
            if (skip_node(node_info, peer_session->remote_node_info_))
                continue;

            peer_session->SendNodeInfo(node_info);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} --> contact: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact);
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
            auto [iter, inserted] =
              server_peer_sessions_.try_emplace(conn_id, std::make_shared<PeerSession>(true, conn_id, config_, node_info_, std::move(peer), *this));

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