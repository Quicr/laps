#pragma once

#include "client_session.h"
#include "metrics_publisher.h"
#include "state.h"

#include "track_ranking.h"
#include <peering/peer_manager.h>
#include <quicr/containers/cache.h>
#include <quicr/messages/object.h>
#include <quicr/session.h>
#include <quicr/session_manager.h>
#include <quicr/utilities/bytes.h>

#include <functional>
#include <mutex>
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
    class ClientManager
    {
      public:
        ClientManager(State& state,
                      const Config& config,
                      const quicr::ServerConfig& cfg,
                      peering::PeerManager& peer_manager,
                      size_t cache_duration_ms = 60000);
        ~ClientManager();

        quicr::Session::Status Start();
        void Stop();

        // -------------------------------------------------------------------------------
        // Per-connection callbacks, invoked by ClientSession
        // -------------------------------------------------------------------------------

        void SubscribeTracksReceived(std::uint64_t connection_handle,
                                     std::uint64_t data_ctx_id,
                                     const quicr::TrackNamespace& prefix_namespace,
                                     const quicr::SubscribeNamespaceAttributes& attributes);

        void UnsubscribeNamespaceReceived(std::uint64_t connection_handle,
                                          const quicr::TrackNamespace& prefix_namespace);

        std::vector<std::uint64_t> PublishNamespaceDoneReceived(std::uint64_t connection_handle,
                                                                std::uint64_t request_id);

        void PublishNamespaceReceived(std::uint64_t connection_handle,
                                      const quicr::TrackNamespace& track_namespace,
                                      const quicr::PublishNamespaceAttributes&);

        void ClientSetupReceived(std::uint64_t connection_handle,
                                 const quicr::ClientSetupAttributes& client_setup_attributes);

        void UnsubscribeReceived(std::uint64_t connection_handle, uint64_t request_id);
        void PublishDoneReceived(std::uint64_t connection_handle, uint64_t request_id);

        void SubscribeReceived(std::uint64_t connection_handle,
                               uint64_t request_id,
                               const quicr::FullTrackName& track_full_name,
                               const quicr::SubscribeAttributes&);

        void NewGroupRequested(const quicr::FullTrackName& track_full_name, std::uint64_t group_id);

        void TrackStatusReceived(std::uint64_t connection_handle,
                                 uint64_t request_id,
                                 const quicr::FullTrackName& track_full_name);

        std::optional<quicr::messages::Location> GetLargestAvailable(const quicr::FullTrackName& track_name);

        void FetchCancelReceived(std::uint64_t connection_handle, uint64_t request_id);

        void StandaloneFetchReceived(std::uint64_t connection_handle,
                                     uint64_t request_id,
                                     const quicr::FullTrackName& track_full_name,
                                     const quicr::StandaloneFetchAttributes& attributes);

        void JoiningFetchReceived(std::uint64_t connection_handle,
                                  uint64_t request_id,
                                  const quicr::FullTrackName& track_full_name,
                                  const quicr::JoiningFetchAttributes& attributes);

        void PublishReceived(std::uint64_t connection_handle,
                             uint64_t request_id,
                             const quicr::PublishAttributes& publish_attributes,
                             std::weak_ptr<quicr::SubscribeNamespaceHandler> ns_handler);

        void MetricsSampled(std::uint64_t connection_handle, const quicr::ConnectionMetrics& metrics);

        /**
         * @brief Tear down all relay state belonging to a closed connection
         */
        void ConnectionClosed(std::uint64_t connection_handle);

        // -------------------------------------------------------------------------------
        // Session API by connection handle
        //
        // libquicr owns one quicr::Session per connection. These forward to the session for the given
        // connection handle so relay code can stay connection-explicit.
        // -------------------------------------------------------------------------------

        void SubscribeTrack(std::uint64_t connection_handle, std::shared_ptr<quicr::SubscribeTrackHandler> handler);

        void UnsubscribeTrack(std::uint64_t connection_handle,
                              const std::shared_ptr<quicr::SubscribeTrackHandler>& handler);

        void UpdateTrackSubscription(std::uint64_t connection_handle,
                                     std::shared_ptr<quicr::SubscribeTrackHandler> handler);

        void PublishNamespace(std::uint64_t connection_handle,
                              std::shared_ptr<quicr::PublishNamespaceHandler> handler,
                              bool passive = false);

        void FetchTrack(std::uint64_t connection_handle, std::shared_ptr<quicr::FetchTrackHandler> handler);

        void CancelFetchTrack(std::uint64_t connection_handle, std::shared_ptr<quicr::FetchTrackHandler> handler);

        void BindPublisherTrack(std::uint64_t connection_handle,
                                std::uint64_t src_id,
                                uint64_t request_id,
                                const std::shared_ptr<quicr::PublishTrackHandler>& handler,
                                bool ephemeral = false);

        void UnbindPublisherTrack(std::uint64_t connection_handle,
                                  std::uint64_t src_id,
                                  const std::shared_ptr<quicr::PublishTrackHandler>& handler,
                                  bool send_publish_done = false);

        void BindFetchTrack(std::uint64_t connection_handle, std::shared_ptr<quicr::PublishFetchHandler> handler);

        void UnbindFetchTrack(std::uint64_t connection_handle,
                              const std::shared_ptr<quicr::PublishFetchHandler>& handler);

        void ResolvePublish(std::uint64_t connection_handle,
                            uint64_t request_id,
                            const quicr::PublishAttributes& attributes,
                            const quicr::PublishResponse& response,
                            std::shared_ptr<quicr::SubscribeTrackHandler> handler);

        void ResolveSubscribe(std::uint64_t connection_handle,
                              uint64_t request_id,
                              uint64_t track_alias,
                              const quicr::RequestResponse& response);

        void ResolveSubscribeTracks(std::uint64_t connection_handle,
                                    std::uint64_t data_ctx_id,
                                    uint64_t request_id,
                                    const quicr::TrackNamespace& prefix,
                                    const quicr::SubscribeNamespaceResponse& response);

        void ResolveFetch(std::uint64_t connection_handle,
                          uint64_t request_id,
                          std::uint8_t priority,
                          std::optional<quicr::messages::GroupOrder> group_order,
                          const quicr::FetchResponse& response);

        void ResolvePublishNamespace(std::uint64_t connection_handle,
                                     uint64_t request_id,
                                     const quicr::TrackNamespace& track_namespace,
                                     const quicr::Session::PublishNamespaceResponse& response);

        void ResolveTrackStatus(std::uint64_t connection_handle,
                                uint64_t request_id,
                                const quicr::RequestResponse& response);

        const std::shared_ptr<timeq::tick_service>& GetTickService() const noexcept { return tick_service_; }

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

      private:
        /**
         * @brief Session for a connection handle, or nullptr when the connection is gone
         */
        std::shared_ptr<ClientSession> GetSession(std::uint64_t connection_handle) const;

        /**
         * @brief Set up relay state for a newly accepted connection
         */
        void NewConnectionAccepted(const std::shared_ptr<ClientSession>& session);

        /**
         * @brief Send a publish namespace to every connection that subscribed to a matching prefix
         *
         * @details libquicr's ResolvePublishNamespace() only mirrors the namespace back on the announcing
         *      connection, so the relay fans it out to the other prefix subscribers itself.
         *
         * @param track_namespace       Namespace that was announced
         * @param exclude_connection    Announcing connection, which is resolved separately
         */
        void FanoutPublishNamespace(const quicr::TrackNamespace& track_namespace, std::uint64_t exclude_connection);

        /**
         * @brief Withdraw a fanned out publish namespace from every connection it was sent to
         */
        void FanoutPublishNamespaceDone(const quicr::TrackNamespace& track_namespace);

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

        /**
         * @brief Binds libquicr's per-connection sessions to the relay
         *
         * @details The session manager owns its callbacks, so these are kept in a separate object rather than
         *      requiring ClientManager itself to be held by a shared pointer.
         */
        class SessionCallbacks : public quicr::SessionManager::Callbacks
        {
          public:
            explicit SessionCallbacks(ClientManager& manager)
              : manager_(manager)
            {
            }

            std::shared_ptr<quicr::Session> CreateServerSession(
              const quicr::ServerConfig& cfg,
              std::shared_ptr<quicr::Transport> transport,
              std::shared_ptr<quicr::Connection> connection,
              std::shared_ptr<timeq::tick_service> tick_service) override;

            void OnNewServerSession(const std::shared_ptr<quicr::Session>& session) override;

            void OnSessionRemoved(const std::shared_ptr<quicr::Session>& session) override;

          private:
            ClientManager& manager_;
        };

        State& state_;
        const Config& config_;
        const quicr::ServerConfig server_config_;
        peering::PeerManager& peer_manager_;
        std::shared_ptr<timeq::tick_service> tick_service_;
        MetricsPublisher metrics_publisher_;

        /// Declared before the session manager, which takes a reference to it for the lifetime of the relay
        std::shared_ptr<SessionCallbacks> session_callbacks_;

        quicr::SessionManager session_manager_;

        /// Listening transport for client connections, owned by the session manager
        std::weak_ptr<quicr::Transport> server_transport_;

        /**
         * @brief Sessions by connection handle
         *
         * @details The session manager owns the sessions, one per client connection. These are weak so that
         *      dropping an entry from a session callback can never destroy the session that is running it.
         */
        mutable std::mutex sessions_mutex_;
        std::map<std::uint64_t, std::weak_ptr<ClientSession>> sessions_;

        /// Fanned out publish namespaces, key is announced namespace and the subscriber connection handle
        std::map<std::pair<quicr::TrackNamespace, std::uint64_t>, std::shared_ptr<quicr::PublishNamespaceHandler>>
          fanout_namespaces_;

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
