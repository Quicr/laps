#pragma once

#include "client_manager.h"
#include "metrics_publisher.h"
#include "state.h"

#include "track_ranking.h"
#include <peering/peer_manager.h>
#include <quicr/containers/cache.h>
#include <quicr/messages/object.h>
#include <quicr/session.h>
#include <quicr/session_callbacks.h>
#include <quicr/session_manager.h>
#include <quicr/utilities/bytes.h>

#include <functional>
#include <memory>
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
      : public quicr::Session::ServerCallbacks
      , public quicr::SessionManager::Callbacks
      , public std::enable_shared_from_this<ClientManager>
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

        std::optional<quicr::messages::Location> GetLargestAvailable(const quicr::FullTrackName& track_name);

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

        const std::shared_ptr<timeq::tick_service>& GetTickService() const noexcept { return tick_service_; }

        /**
         * @brief Register a relay-local publish (e.g. the internally generated metrics track)
         *
         * @details Unlike PublishReceived(), this is never treated as peer-originated even though it uses
         *      a nullptr session and request_id=0 internally. Use this instead of calling
         *      PublishReceived(nullptr, 0, ...) directly for relay-local publishes.
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

        friend class peering::PeerManager;

      private:
        /**
         * @brief Connection handle of the session that received a request, or zero if it is already gone
         */
        static std::uint64_t ConnectionHandle(const std::shared_ptr<quicr::Session>& session);

        // -- quicr::Session::ServerCallbacks -------------------------------------------------------

        void MetricsSampled(const std::shared_ptr<quicr::Session>& session,
                            const quicr::ConnectionMetrics& metrics) override;

        quicr::Reply<void, quicr::ErrorCode> ClientSetupReceived(
          const std::shared_ptr<quicr::Session>& session,
          const quicr::ClientSetupAttributes& client_setup_attributes) override;

        quicr::Reply<std::vector<quicr::TrackNamespace>, quicr::RequestErrorCode> SubscribeTracksReceived(
          const std::shared_ptr<quicr::Session>& session,
          const quicr::TrackNamespace& prefix_namespace,
          const quicr::SubscribeNamespaceAttributes& attributes) override;

        quicr::Reply<void, quicr::ErrorCode> UnsubscribeNamespaceReceived(
          const std::shared_ptr<quicr::Session>& session,
          const quicr::TrackNamespace& prefix_namespace) override;

        quicr::Reply<void, quicr::PublishNamespaceErrorCode> PublishNamespaceReceived(
          const std::shared_ptr<quicr::Session>& session,
          const quicr::TrackNamespace& track_namespace,
          const quicr::PublishNamespaceAttributes& attributes) override;

        quicr::Reply<void, quicr::PublishNamespaceErrorCode> PublishNamespaceDoneReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id) override;

        quicr::Reply<const quicr::PublishResponse, quicr::PublishErrorCode> PublishReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id,
          const quicr::PublishAttributes& publish_attributes,
          std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler) override;

        quicr::Reply<void, quicr::ErrorCode> PublishDoneReceived(const std::shared_ptr<quicr::Session>& session,
                                                    std::uint64_t request_id) override;

        quicr::Reply<quicr::RequestResponse, quicr::RequestErrorCode> SubscribeReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id,
          const quicr::FullTrackName& track_full_name,
          const quicr::SubscribeAttributes& subscribe_attributes) override;

        quicr::Reply<void, quicr::ErrorCode> UnsubscribeReceived(const std::shared_ptr<quicr::Session>& session,
                                                    std::uint64_t request_id) override;

        quicr::Reply<void, quicr::ErrorCode> NewGroupRequested(const quicr::FullTrackName& track_full_name,
                                                  std::uint64_t group_id) override;

        quicr::Reply<quicr::RequestResponse, quicr::RequestErrorCode> TrackStatusReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id,
          const quicr::FullTrackName& track_full_name) override;

        quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> StandaloneFetchReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id,
          const quicr::FullTrackName& track_full_name,
          const quicr::StandaloneFetchAttributes& attributes) override;

        quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> JoiningFetchReceived(
          const std::shared_ptr<quicr::Session>& session,
          std::uint64_t request_id,
          const quicr::FullTrackName& track_full_name,
          const quicr::JoiningFetchAttributes& attributes) override;

        quicr::Reply<void, quicr::FetchErrorCode> FetchCancelReceived(const std::shared_ptr<quicr::Session>& session,
                                                                      std::uint64_t request_id) override;

        // -- quicr::SessionManager::Callbacks ------------------------------------------------------

        void OnNewServerSession(const std::shared_ptr<quicr::Session>& new_session) override;

        void OnSessionRemoved(const std::shared_ptr<quicr::Session>& session) override;

        /**
         * @brief Session for a connection handle, or nullptr when the connection is gone
         */
        std::shared_ptr<quicr::Session> GetSession(std::uint64_t connection_handle) const;

        /**
         * @brief Set up relay state for a newly accepted connection
         */
        void NewConnectionAccepted(const std::shared_ptr<quicr::Session>& session);

        /**
         * @brief Send a publish namespace to every connection that subscribed to a matching prefix
         *
         * @details Accepting the announcement only mirrors the namespace back on the announcing connection, so
         *      the relay fans it out to the other prefix subscribers itself.
         *
         * @param track_namespace       Namespace that was announced
         * @param exclude_connection    Announcing connection, which libquicr handles when it accepts
         */
        void FanoutPublishNamespace(const quicr::TrackNamespace& track_namespace, std::uint64_t exclude_connection);

        /**
         * @brief Withdraw a fanned out publish namespace from every connection it was sent to
         */
        void FanoutPublishNamespaceDone(const quicr::TrackNamespace& track_namespace);

        quicr::Reply<const quicr::PublishResponse, quicr::PublishErrorCode> PublishReceivedInternal(
          std::uint64_t connection_handle,
          uint64_t request_id,
          const quicr::PublishAttributes& publish_attributes,
          bool is_from_peer);

        void PurgePublishState(std::uint64_t connection_handle);

        quicr::Reply<const quicr::FetchResponse, quicr::FetchErrorCode> FetchReceived(
          std::uint64_t connection_handle,
          uint64_t request_id,
          const quicr::FullTrackName& track_full_name,
          uint8_t priority,
          std::optional<quicr::messages::GroupOrder> group_order,
          quicr::messages::Location start,
          quicr::messages::FetchEndLocation end);

        State& state_;
        const Config& config_;
        const quicr::ServerConfig server_config_;
        peering::PeerManager& peer_manager_;
        std::shared_ptr<timeq::tick_service> tick_service_;
        MetricsPublisher metrics_publisher_;

        std::unique_ptr<quicr::SessionManager> session_manager_;

        /**
         * @brief Weak references to sessions by connection handle
         *
         * @details SessionManager owns the sessions. The relay only keeps weak references so it can look up a
         *      session for a connection it is not currently handling a request for.
         */
        mutable std::mutex sessions_mutex_;
        std::map<std::uint64_t, std::weak_ptr<quicr::Session>> sessions_;

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
