#pragma once

#include "metrics_publisher.h"
#include "state.h"

#include "track_ranking.h"
#include <peering/peer_manager.h>
#include <quicr/containers/cache.h>
#include <quicr/messages/object.h>
#include <quicr/session.h>
#include <quicr/utilities/bytes.h>

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
    class ClientManager : public quicr::Session
    {
      public:
        ClientManager(State& state,
                      const Config& config,
                      const quicr::ServerConfig& cfg,
                      peering::PeerManager& peer_manager,
                      size_t cache_duration_ms = 60000);
        ~ClientManager();

        quicr::Session::Status Start() override;
        void Stop() override;

        void NewConnectionAccepted(std::uint64_t connection_handle, const ConnectionRemoteInfo& remote) override;

        void SubscribeTracksReceived(std::uint64_t connection_handle,
                                     std::uint64_t data_ctx_id,
                                     const quicr::TrackNamespace& prefix_namespace,
                                     const quicr::SubscribeNamespaceAttributes& attributes) override;

        void UnsubscribeNamespaceReceived(std::uint64_t connection_handle,
                                          const quicr::TrackNamespace& prefix_namespace) override;

        std::vector<std::uint64_t> PublishNamespaceDoneReceived(std::uint64_t connection_handle,
                                                                std::uint64_t request_id) override;

        void PublishNamespaceReceived(std::uint64_t connection_handle,
                                      const quicr::TrackNamespace& track_namespace,
                                      const quicr::PublishNamespaceAttributes&) override;

        void ConnectionStatusChanged(std::uint64_t connection_handle, ConnectionStatus status) override;

        void ClientSetupReceived(std::uint64_t, const quicr::ClientSetupAttributes& client_setup_attributes) override;

        void UnsubscribeReceived(std::uint64_t connection_handle, uint64_t request_id) override;
        void PublishDoneReceived(std::uint64_t connection_handle, uint64_t request_id) override;

        void SubscribeReceived(std::uint64_t connection_handle,
                               uint64_t request_id,
                               const quicr::FullTrackName& track_full_name,
                               const quicr::SubscribeAttributes&) override;

        void NewGroupRequested(const quicr::FullTrackName& track_full_name, std::uint64_t group_id) override;

        void TrackStatusReceived(std::uint64_t connection_handle,
                                 uint64_t request_id,
                                 const quicr::FullTrackName& track_full_name) override;

        std::optional<quicr::messages::Location> GetLargestAvailable(const quicr::FullTrackName& track_name);

        void FetchCancelReceived(std::uint64_t connection_handle, uint64_t request_id) override;

        void StandaloneFetchReceived(std::uint64_t connection_handle,
                                     uint64_t request_id,
                                     const quicr::FullTrackName& track_full_name,
                                     const quicr::StandaloneFetchAttributes& attributes) override;

        void JoiningFetchReceived(std::uint64_t connection_handle,
                                  uint64_t request_id,
                                  const quicr::FullTrackName& track_full_name,
                                  const quicr::JoiningFetchAttributes& attributes) override;

        void PublishReceived(std::uint64_t connection_handle,
                             uint64_t request_id,
                             const quicr::PublishAttributes& publish_attributes,
                             std::weak_ptr<quicr::SubscribeNamespaceHandler> ns_handler) override;

        /**
         * @brief Register a relay-local publish (e.g. the internally generated metrics track)
         *
         * @details Unlike PublishReceived(), this is never treated as peer-originated even though it uses
         *      the same connection_handle=0/request_id=0 sentinel internally. Use this instead of calling
         *      PublishReceived(0, 0, ...) directly for relay-local publishes.
         */
        void RegisterLocalPublish(const quicr::PublishAttributes& publish_attributes);

        void ProcessSubscribe(std::uint64_t connection_handle,
                              uint64_t request_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::SubscribeAttributes&,
                              std::optional<quicr::messages::Location>);

        bool PublishLocalObject(std::uint64_t track_fullname_hash,
                                const quicr::ObjectHeaders& object_headers,
                                quicr::BytesSpan data);

        void PeerDataReceived(std::uint64_t track_full_name_hash,
                              bool is_new_stream,
                              std::optional<uint64_t> stream_id,
                              std::shared_ptr<const std::vector<uint8_t>> data);

        void PeerUnsubscribeTrack(std::uint64_t track_full_name_hash);

        void PeerStreamClosed(std::uint64_t track_full_name_hash, uint64_t stream_id, bool reset);

        bool DampenOrUpdateTrackSubscription(std::shared_ptr<SubscribeTrackHandler> sub_to_pub_track_handler,
                                             bool new_group_request);

        void RemoveOrPausePublisherSubscribe(std::uint64_t track_fullname_hash);

        void MetricsSampled(const std::uint64_t connection_handle, const quicr::ConnectionMetrics& metrics) override;

      private:
        void PublishReceivedInternal(std::uint64_t connection_handle,
                                     uint64_t request_id,
                                     const quicr::PublishAttributes& publish_attributes,
                                     bool is_from_peer);

        void PurgePublishState(std::uint64_t connection_handle);

        void FetchReceived(std::uint64_t connection_handle,
                           uint64_t request_id,
                           const quicr::FullTrackName& track_full_name,
                           uint8_t priority,
                           std::optional<quicr::messages::GroupOrder> group_order,
                           quicr::messages::Location start,
                           quicr::messages::FetchEndLocation end);

        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;
        MetricsPublisher metrics_publisher_;

        /**
         * @brief Map of atomic bools to mark if a fetch thread should be interrupted.
         */
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::atomic_bool> stop_fetch_;

        size_t cache_duration_ms_ = 0;
        std::map<std::uint64_t, quicr::Cache<std::uint64_t, std::set<CacheObject>>> cache_;

        // Key is track namespace hash
        std::unordered_map<std::uint64_t, std::shared_ptr<TrackRanking>> track_rankings_;

        friend class SubscribeTrackHandler;
        friend class PublishTrackHandler;
        friend class FetchTrackHandler;
    };
} // namespace laps
