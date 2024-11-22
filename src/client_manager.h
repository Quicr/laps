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
        uint8_t priority;
        uint32_t ttl;
        bool stream_header_needed;
        uint64_t group_id;
        uint64_t subgroup_id;
        uint64_t object_id;
        std::optional<quicr::Extensions> extensions;
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
        return lhs.object_id < rhs.object_id;
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
                      peering::PeerManager& peer_manager);

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

        bool FetchReceived(quicr::ConnectionHandle connection_handle,
                           uint64_t subscribe_id,
                           const quicr::FullTrackName& track_full_name,
                           const quicr::FetchAttributes& attrs) override;

        void OnFetchOk(quicr::ConnectionHandle connection_handle,
                       uint64_t subscribe_id,
                       const quicr::FullTrackName& track_full_name,
                       const quicr::FetchAttributes& attrs) override;

        void FetchCancelReceived(quicr::ConnectionHandle connection_handle, uint64_t subscribe_id) override;

        void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                              uint64_t subscribe_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::SubscribeAttributes&);

        void ProcessPeerDataObject(const peering::DataObject& data_object);

        void RemovePublisherSubscribe(const quicr::TrackHash& track_hash);

        void MetricsSampled(const quicr::ConnectionHandle connection_handle,
                            const quicr::ConnectionMetrics& metrics) override;

      private:
        void PurgePublishState(quicr::ConnectionHandle connection_handle);

        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;

        std::map<quicr::TrackHash, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>> cache_;

        friend class SubscribeTrackHandler;
        friend class PublishTrackHandler;
        friend class FetchTrackHandler;
    };
} // namespace laps
