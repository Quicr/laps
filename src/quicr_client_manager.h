#pragma once

#include "client_manager.h"
#include "peering/peer_manager.h"
#include "quicr_moq_server_port.h"
#include "relay_core.h"
#include "state.h"
#include "track_ranking.h"

#include <quicr/cache.h>
#include <quicr/server.h>

#include <functional>
#include <set>

/**
 * @brief Specialization of std::less for sorting CacheObjects by object ID.
 */
template<>
struct std::less<quicr::TrackHash>
{
    constexpr bool operator()(const quicr::TrackHash& lhs, const quicr::TrackHash& rhs) const noexcept
    {
        return lhs.track_fullname_hash < rhs.track_fullname_hash;
    }
};

namespace laps {
    /**
     * @brief MoQ Server
     * @details Implementation of the MoQ Server
     */
    class QuicrClientManager
      : public ClientManager
      , public quicr::Server
    {
      public:
        QuicrClientManager(State& state,
                           const Config& config,
                           const quicr::ServerConfig& cfg,
                           peering::PeerManager& peer_manager);

        bool Start() override { return quicr::Server::Start() == Status::kReady; }

        void NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                   const ConnectionRemoteInfo& remote) override;

        void SubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                        quicr::DataContextId data_ctx_id,
                                        const quicr::TrackNamespace& prefix_namespace,
                                        const quicr::messages::SubscribeNamespaceAttributes& attributes) override;

        void UnsubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                          const quicr::TrackNamespace& prefix_namespace) override;

        std::vector<quicr::ConnectionHandle> PublishNamespaceDoneReceived(
          quicr::ConnectionHandle connection_handle,
          quicr::messages::RequestID request_id) override;

        void PublishNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                      const quicr::TrackNamespace& track_namespace,
                                      const quicr::PublishNamespaceAttributes&) override;

        void ConnectionStatusChanged(quicr::ConnectionHandle connection_handle, ConnectionStatus status) override;

        ClientSetupResponse ClientSetupReceived(quicr::ConnectionHandle,
                                                const quicr::ClientSetupAttributes& client_setup_attributes) override;

        void UnsubscribeReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id) override;
        void PublishDoneReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id) override;

        void SubscribeReceived(quicr::ConnectionHandle connection_handle,
                               uint64_t request_id,
                               const quicr::FullTrackName& track_full_name,
                               const quicr::messages::SubscribeAttributes&) override;

        void NewGroupRequested(const quicr::FullTrackName& track_full_name, quicr::messages::GroupId group_id) override;

        void TrackStatusReceived(quicr::ConnectionHandle connection_handle,
                                 uint64_t request_id,
                                 const quicr::FullTrackName& track_full_name) override;

        std::optional<quicr::messages::Location> GetLargestAvailable(const quicr::FullTrackName& track_name);

        void FetchCancelReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id) override;

        void StandaloneFetchReceived(quicr::ConnectionHandle connection_handle,
                                     uint64_t request_id,
                                     const quicr::FullTrackName& track_full_name,
                                     const quicr::messages::StandaloneFetchAttributes& attributes) override;

        void JoiningFetchReceived(quicr::ConnectionHandle connection_handle,
                                  uint64_t request_id,
                                  const quicr::FullTrackName& track_full_name,
                                  const quicr::messages::JoiningFetchAttributes& attributes) override;

        void PublishReceived(quicr::ConnectionHandle connection_handle,
                             uint64_t request_id,
                             const quicr::messages::PublishAttributes& publish_attributes,
                             std::weak_ptr<quicr::SubscribeNamespaceHandler> ns_handler) override;

        void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                              uint64_t request_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::messages::SubscribeAttributes&,
                              std::optional<quicr::messages::Location>) override;

        bool DampenOrUpdateTrackSubscription(std::shared_ptr<SubscribeTrackHandler> sub_to_pub_track_handler,
                                             bool new_group_request);

        void RemoveOrPausePublisherSubscribe(quicr::TrackFullNameHash track_fullname_hash) override;

        void ApplyPeerAnnouncePublishNamespace(quicr::ConnectionHandle connection_handle,
                                               const quicr::TrackNamespace& track_namespace,
                                               const quicr::PublishNamespaceAttributes& attributes) override;

        void ApplyPeerAnnouncePublishNamespaceDone(quicr::ConnectionHandle connection_handle,
                                                   quicr::messages::RequestID request_id) override;

        void ApplyPeerAnnouncePublish(quicr::ConnectionHandle connection_handle,
                                      uint64_t request_id,
                                      const quicr::messages::PublishAttributes& publish_attributes,
                                      std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler) override;

        void RelayBindPublisherTrack(quicr::ConnectionHandle connection_handle,
                                     quicr::ConnectionHandle src_id,
                                     uint64_t request_id,
                                     const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
                                     bool ephemeral) override;

        void RelayUnbindPublisherTrack(quicr::ConnectionHandle connection_handle,
                                       quicr::ConnectionHandle src_id,
                                       const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
                                       bool send_publish_done) override;

        void MetricsSampled(const quicr::ConnectionHandle connection_handle,
                            const quicr::ConnectionMetrics& metrics) override;

      private:
        void PurgePublishState(quicr::ConnectionHandle connection_handle);

        void FetchReceived(quicr::ConnectionHandle connection_handle,
                           uint64_t request_id,
                           const quicr::FullTrackName& track_full_name,
                           uint8_t priority,
                           quicr::messages::GroupOrder group_order,
                           quicr::messages::Location start,
                           quicr::messages::FetchEndLocation end);

        std::shared_ptr<moq::shim::QuicrMoqServerPort> quicr_moq_port_;
        moq::shim::RelayCore relay_core_;

        friend class FetchTrackHandler;
    };
} // namespace laps
