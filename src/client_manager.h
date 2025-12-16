#pragma once

#include "state.h"

#include <peering/peer_manager.h>
#include <quicr/cache.h>
#include <quicr/server.h>

#include <functional>
#include <set>

namespace laps {
    /**
     * @brief Defines an object received from an announcer that lives in the cache.
     */
    struct CacheObject
    {
        quicr::ObjectHeaders headers;
        quicr::Bytes data;
    };
}

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

template<>
struct std::less<laps::CacheObject>
{
    constexpr bool operator()(const laps::CacheObject& lhs, const laps::CacheObject& rhs) const noexcept
    {
        return lhs.headers.object_id < rhs.headers.object_id;
    }
};

namespace laps {
    /**
     * @brief MoQ Server
     * @details Implementation of the MoQ Server
     */
    class ClientManager : public quicr::Server
    {
      public:
        ClientManager(State& state,
                      const Config& config,
                      const quicr::ServerConfig& cfg,
                      peering::PeerManager& peer_manager,
                      size_t cache_duration_ms = 60000);

        void NewConnectionAccepted(quicr::ConnectionHandle connection_handle,
                                   const ConnectionRemoteInfo& remote) override;

        void SubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                        const quicr::TrackNamespace& prefix_namespace,
                                        const quicr::SubscribeNamespaceAttributes& attributes) override;

        void UnsubscribeNamespaceReceived(quicr::ConnectionHandle connection_handle,
                                          const quicr::TrackNamespace& prefix_namespace) override;

        std::vector<quicr::ConnectionHandle> PublishNamespaceDoneReceived(
          quicr::ConnectionHandle connection_handle,
          const quicr::TrackNamespace& track_namespace) override;

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
                                 const quicr::FullTrackName& track_full_name,
                                 const quicr::messages::SubscribeAttributes& subscribe_attributes) override;

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
                             const quicr::messages::PublishAttributes& publish_attributes) override;

        void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                              uint64_t request_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::messages::SubscribeAttributes&,
                              std::optional<quicr::messages::Location>);

        bool DampenOrUpdateTrackSubscription(std::shared_ptr<SubscribeTrackHandler> sub_to_pub_track_handler,
                                             bool new_group_request);

        void RemoveOrPausePublisherSubscribe(const quicr::TrackHash& track_hash);

        void MetricsSampled(const quicr::ConnectionHandle connection_handle,
                            const quicr::ConnectionMetrics& metrics) override;

      private:
        void PurgePublishState(quicr::ConnectionHandle connection_handle);

        void FetchReceived(quicr::ConnectionHandle connection_handle,
                           uint64_t request_id,
                           const quicr::FullTrackName& track_full_name,
                           quicr::messages::SubscriberPriority priority,
                           quicr::messages::GroupOrder group_order,
                           quicr::messages::Location start,
                           std::optional<quicr::messages::Location> end);

        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;

        /**
         * @brief Map of atomic bools to mark if a fetch thread should be interrupted.
         */
        std::map<std::pair<quicr::ConnectionHandle, quicr::messages::RequestID>, std::atomic_bool> stop_fetch_;

        size_t cache_duration_ms_ = 0;
        std::map<quicr::TrackFullNameHash, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>> cache_;

        friend class SubscribeTrackHandler;
        friend class PublishTrackHandler;
        friend class FetchTrackHandler;
    };
} // namespace laps
