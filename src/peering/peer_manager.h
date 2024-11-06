// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <map>
#include <quicr/detail/quic_transport.h>
#include <quicr/detail/safe_queue.h>
#include <thread>
#include <vector>

#include "config.h"
#include "state.h"
#include "info_base.h"
#include "peer_session.h"

namespace laps::peering {
    /**
     * @brief Peering manager class. Manages relay to relay (peering) forwarding of
     *      subscriber objects.
     */
    class PeerManager : public quicr::ITransport::TransportDelegate
    {
      public:
        friend class PeerSession;

        PeerManager(const Config& cfg, State& state, std::shared_ptr<InfoBase> info_base);
        ~PeerManager();

        // -------------------------------------------------------------------------------
        // Methods used by peer session and client manager to feedback/update manager
        // -------------------------------------------------------------------------------

        void NodeReceived(PeerSessionId peer_session_id, const NodeInfo& node_info, bool remove);
        void SessionChanged(PeerSessionId peer_session_id,
                            PeerSession::StatusValue status,
                            const NodeInfo& remote_node_info);

        void

        // -------------------------------------------------------------------------------
        // QUIC Transport callbacks
        // -------------------------------------------------------------------------------

        void OnNewDataContext(const quicr::TransportConnId& conn_id, const quicr::DataContextId& data_ctx_id) override
        {
        }
        void OnConnectionStatus(const quicr::TransportConnId& conn_id, const quicr::TransportStatus status) override;
        void OnNewConnection(const quicr::TransportConnId& conn_id, const quicr::TransportRemote& remote) override;
        void OnRecvStream(const quicr::TransportConnId& conn_id,
                          uint64_t stream_id,
                          std::optional<quicr::DataContextId> data_ctx_id,
                          const bool is_bidir = false) override;
        void OnRecvDgram(const quicr::TransportConnId& conn_id,
                         std::optional<quicr::DataContextId> data_ctx_id) override;

        // -------------------------------------------------------------------------------

      private:
        /**
         * @brief Check Thread to perform reconnects and cleanup
         * @details Thread will perform various tasks each interval
         *
         * @param interval_ms       Interval in milliseconds to sleep before running check
         */
        void CheckThread(int interval_ms);

        /**
         * @brief Create a peering session/connection
         *
         * @param peer_config           Peer/relay configuration parameters
         *
         */
        void CreatePeerSession(const quicr::TransportRemote& peer_config);

        void PropagateNodeInfo(const NodeInfo& node_info);
        void PropagateNodeInfo(PeerSessionId peer_session_id, const NodeInfo& node_info);
        std::shared_ptr<PeerSession> GetPeerSession(PeerSessionId peer_session_id);

      private:
        bool stop_{ false };
        std::mutex mutex_;
        std::shared_ptr<InfoBase> info_base_;
        std::shared_ptr<quicr::TickService> tick_service_;
        const Config& config_;
        State& state_;
        NodeInfo node_info_;

        /// Peer sessions that are accepted by the server
        std::map<PeerSessionId, std::shared_ptr<PeerSession>> server_peer_sessions_;

        /// Peer sessions that are initiated by the peer manager
        std::map<PeerSessionId, std::shared_ptr<PeerSession>> client_peer_sessions_;

        std::shared_ptr<quicr::ITransport> server_transport_; /// Server Transport for inbound connections

        std::thread check_thr_; /// Check/task thread, handles reconnects
    };

} // namespace laps