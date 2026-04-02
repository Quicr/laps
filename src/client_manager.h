#pragma once

#include "config.h"
#include "peering/peer_manager.h"
#include "state.h"
#include "track_ranking.h"
#include "types.h"

#include <quicr/cache.h>
#include <quicr/common.h>
#include <quicr/detail/attributes.h>
#include <quicr/object.h>
#include <quicr/publish_track_handler.h>
#include <quicr/track_name.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace quicr {
    class PublishFetchHandler;
    class SubscribeNamespaceHandler;
}

namespace laps {

    struct CacheObject
    {
        quicr::ObjectHeaders headers;
        quicr::Bytes data;
    };

    class SubscribeTrackHandler;
    class PublishTrackHandler;
    class PublishFetchHandler;

    class ClientManager
    {
      public:
        ClientManager(State& state, const Config& config, peering::PeerManager& peer_manager);

        const Config& GetConfig() const noexcept { return config_; }

        virtual bool Start() = 0;

        /**
         * @brief Apply subscribe policy for peer fan-in (connection_handle/request_id 0) or internal paths.
         *        Default: no-op (QuicrClientManager / MoxygenClientManager override).
         */
        virtual void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                      uint64_t request_id,
                                      const quicr::TrackHash& th,
                                      const quicr::FullTrackName& track_full_name,
                                      const quicr::messages::SubscribeAttributes& attrs,
                                      std::optional<quicr::messages::Location> largest);

        /**
         * @brief Tear down publisher-side subscribe when no peering subscribers remain.
         *        Default: no-op.
         */
        virtual void RemoveOrPausePublisherSubscribe(quicr::TrackFullNameHash track_fullname_hash);

        /** @brief Peering: remote relay announced a publish namespace (synthetic connection_handle, often 0). */
        virtual void ApplyPeerAnnouncePublishNamespace(quicr::ConnectionHandle connection_handle,
                                                       const quicr::TrackNamespace& track_namespace,
                                                       const quicr::PublishNamespaceAttributes& attributes);

        virtual void ApplyPeerAnnouncePublishNamespaceDone(quicr::ConnectionHandle connection_handle,
                                                           quicr::messages::RequestID request_id);

        /** @brief Peering: remote relay announced a publish track. */
        virtual void ApplyPeerAnnouncePublish(quicr::ConnectionHandle connection_handle,
                                              uint64_t request_id,
                                              const quicr::messages::PublishAttributes& publish_attributes,
                                              std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler);

        /**
         * @brief Bind publisher track for relay fan-out (libquicr `Server::BindPublisherTrack` on QUICR backend).
         */
        virtual void RelayBindPublisherTrack(quicr::ConnectionHandle connection_handle,
                                           quicr::ConnectionHandle src_id,
                                           uint64_t request_id,
                                           const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
                                           bool ephemeral);

        virtual void RelayUnbindPublisherTrack(quicr::ConnectionHandle connection_handle,
                                               quicr::ConnectionHandle src_id,
                                               const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
                                               bool send_publish_done);

        /**
         * @brief Moxygen relay: publish one complete object via MoQ `TrackConsumer` when `RelayBindPublisherTrack`
         *        registered a binding (no libquicr transport). Returns nullopt to use the default QUICR path.
         */
        virtual std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenRelayPublishObject(
          quicr::PublishTrackHandler* handler,
          quicr::TrackMode track_mode,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data);

        /** @brief Moxygen relay: stream pipeline continuation; nullopt = default path. */
        virtual std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenRelayForwardPublishedData(
          quicr::PublishTrackHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data);

        /** @brief MoQ fetch response path (cache / relay); nullopt if not implemented for this backend. */
        virtual std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchPublishObject(
          quicr::PublishFetchHandler* handler,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data);

        virtual std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchForwardPublishedData(
          quicr::PublishFetchHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data);

      protected:
        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;

        friend class SubscribeTrackHandler;
        friend class PublishTrackHandler;
        friend class PublishFetchHandler;

        /**
         * @brief Map of atomic bools to mark if a fetch thread should be interrupted.
         */
        std::map<std::pair<ConnectionHandle, RequestID>, std::atomic_bool> stop_fetch_;

        std::unordered_map<std::uint64_t, std::shared_ptr<TrackRanking>> track_rankings_;

        std::size_t cache_duration_ms_{};
        std::map<quicr::TrackFullNameHash, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>> cache_;
    };

    std::shared_ptr<ClientManager> MakeClientManager(State& state,
                                                     const Config& config,
                                                     peering::PeerManager& peer_manager);
} // namespace laps

template<>
struct std::less<laps::CacheObject>
{
    constexpr bool operator()(const laps::CacheObject& lhs, const laps::CacheObject& rhs) const noexcept
    {
        return lhs.headers.object_id < rhs.headers.object_id;
    }
};
