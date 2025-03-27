// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <map>
#include <quicr/detail/quic_transport.h>
#include <quicr/detail/safe_queue.h>
#include <thread>

#include "config.h"
#include "info_base.h"
#include "messages/data_header.h"
#include "peer_session.h"
#include "state.h"

namespace laps {
    class ClientManager;
}
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

        void NodeReceived(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw);
        void SubscribeInfoReceived(PeerSessionId peer_session_id, SubscribeInfo& subscribe_info, bool withdraw);
        void AnnounceInfoReceived(PeerSessionId peer_session_id, const AnnounceInfo& announce_info, bool withdraw);

        void SessionChanged(PeerSessionId peer_session_id,
                            PeerSession::StatusValue status,
                            const NodeInfo& remote_node_info);

        void ForwardPeerData(PeerSessionId peer_session_id,
                             bool is_new_stream,
                             uint64_t stream_id,
                             DataHeader data_header,
                             std::shared_ptr<const std::vector<uint8_t>> data,
                             uint64_t data_offset,
                             quicr::ITransport::EnqueueFlags eflags);

        void ClientDataRecv(quicr::TrackFullNameHash track_full_name_hash,
                            uint8_t priority,
                            uint32_t ttl,
                            DataType type,
                            std::shared_ptr<const std::vector<uint8_t>> data);

        void ClientAnnounce(const quicr::FullTrackName& full_track_name,
                            const quicr::PublishAnnounceAttributes&,
                            bool withdraw = false);

        void ClientUnannounce(const quicr::FullTrackName& full_track_name)
        {
            ClientAnnounce(full_track_name, {}, true);
        }

        void ClientSubscribe(const quicr::FullTrackName& track_full_name,
                             const quicr::messages::SubscribeAttributes&,
                             Span<const uint8_t> subscribe_data,
                             bool withdraw = false);

        void ClientUnsubscribe(const quicr::FullTrackName& track_full_name)
        {
            ClientSubscribe(track_full_name, {}, {}, true);
        }

        void SetClientManager(std::shared_ptr<ClientManager> client_manager)
        {
            client_manager_ = std::move(client_manager);
        }

        void InfoBaseSyncPeer(PeerSession& peer_session);

        void SnsReceived(PeerSession& peer_session, const SubscribeNodeSet& sns, bool withdraw = false);

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

        void PropagateNodeInfo(const NodeInfo& node_info, bool withdraw = false);
        void PropagateNodeInfo(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw = false);
        std::shared_ptr<PeerSession> GetPeerSession(PeerSessionId peer_session_id);

      private:
        bool stop_{ false };
        std::mutex mutex_;
        std::shared_ptr<InfoBase> info_base_;
        std::shared_ptr<quicr::TickService> tick_service_;
        std::shared_ptr<ClientManager> client_manager_;
        const Config& config_;
        State& state_;
        NodeInfo node_info_;

        /// Peer sessions that are accepted by the server
        std::map<PeerSessionId, std::shared_ptr<PeerSession>> server_peer_sessions_;

        /// Peer sessions that are initiated by the peer manager
        std::map<PeerSessionId, std::shared_ptr<PeerSession>> client_peer_sessions_;

        std::shared_ptr<quicr::ITransport> server_transport_; /// Server Transport for inbound connections

        std::thread check_thr_; /// Check/task thread, handles reconnects

        /// Subscribe track handler for received data
        std::map<quicr::messages::TrackAlias, std::shared_ptr<SubscribeTrackHandler>> subscribe_handlers_;
    };

} // namespace laps