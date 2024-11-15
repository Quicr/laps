#pragma once

#include "state.h"

#include <peering/peer_manager.h>
#include <quicr/server.h>

namespace laps {
    /**
     * @brief MoQ Server
     * @details Implementation of the MoQ Server
     */
    class ClientManager : public quicr::Server
    {
      public:
        ClientManager(State& state, const quicr::ServerConfig& cfg, peering::PeerManager& peer_manager);

        void NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                   const ConnectionRemoteInfo& remote) override;
        void UnannounceReceived(quicr::ConnectionHandle connection_handle,
                                const quicr::TrackNamespace& track_namespace) override;

        void AnnounceReceived(quicr::ConnectionHandle connection_handle,
                              const quicr::TrackNamespace& track_namespace,
                              const quicr::PublishAnnounceAttributes&) override;

        void ConnectionStatusChanged(quicr::ConnectionHandle connection_handle, ConnectionStatus status) override;

        ClientSetupResponse ClientSetupReceived(quicr::ConnectionHandle,
                                                const quicr::ClientSetupAttributes& client_setup_attributes) override;

        void UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t subscribe_id) override;

        void SubscribeReceived(quicr::ConnectionHandle connection_handle,
                               uint64_t subscribe_id,
                               [[maybe_unused]] uint64_t proposed_track_alias,
                               const quicr::FullTrackName& track_full_name,
                               const quicr::SubscribeAttributes&) override;

        void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                              uint64_t subscribe_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::SubscribeAttributes&);

        void RemovePublisherSubscribe(const quicr::TrackHash& track_hash);

        void MetricsSampled(const quicr::ConnectionHandle connection_handle,
                            const quicr::ConnectionMetrics& metrics) override;

      private:
        void PurgePublishState(quicr::ConnectionHandle connection_handle);

        State& state_;
        peering::PeerManager& peer_manager_;

        friend class SubscribeTrackHandler;
        friend class PublishTrackHandler;
    };
} // namespace laps
