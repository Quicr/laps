#pragma once

#include "moq_server_port.h"

#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

#include <quicr/common.h>
#include <quicr/object.h>
#include <quicr/publish_fetch_handler.h>
#include <quicr/publish_track_handler.h>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <quicr/detail/ctrl_message_types.h>

namespace spdlog {
    class logger;
}

namespace laps {
    class MoxygenClientManager;
}

namespace moxygen {
    class MoQSession;
}

namespace laps::moq::shim {

    /**
     * @brief Moxygen-side `MoqServerPort`: resolves `ConnectionHandle` → `MoQSession` via the manager’s
     *        registry. Logs context at debug; wire I/O (`co_await session->subscribe` / fetch / announce)
     *        is the next integration step (see design doc §10).
     */
    class MoxygenMoqServerPort final : public MoqServerPort
    {
      public:
        MoxygenMoqServerPort(MoxygenClientManager& owner, std::shared_ptr<spdlog::logger> log);
        ~MoxygenMoqServerPort() override;

        void SubscribeTrack(quicr::ConnectionHandle connection_handle,
                            std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) override;

        void UnsubscribeTrack(quicr::ConnectionHandle connection_handle,
                              const std::shared_ptr<quicr::SubscribeTrackHandler>& track_handler) override;

        void UpdateTrackSubscription(quicr::ConnectionHandle connection_handle,
                                     std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) override;

        void PublishNamespace(quicr::ConnectionHandle connection_handle,
                              std::shared_ptr<quicr::PublishNamespaceHandler> ns_handler,
                              bool passive = false) override;

        void BindFetchTrack(quicr::ConnectionHandle connection_handle,
                            std::shared_ptr<quicr::PublishFetchHandler> track_handler) override;

        void UnbindFetchTrack(quicr::ConnectionHandle connection_handle,
                              const std::shared_ptr<quicr::PublishFetchHandler>& track_handler) override;

        void FetchTrack(quicr::ConnectionHandle connection_handle,
                        std::shared_ptr<quicr::FetchTrackHandler> track_handler) override;

        void CancelFetchTrack(quicr::ConnectionHandle connection_handle,
                              std::shared_ptr<quicr::FetchTrackHandler> track_handler) override;

        /** Drop cached upstream subscribe handles when the MoQ session is torn down (no UNSUBSCRIBE on wire). */
        void ClearUpstreamSubscriptionsFor(quicr::ConnectionHandle connection_handle);

        /** Inbound client SUBSCRIBE → `SubscriptionHandle` (for relay PUBLISH toward that subscriber). */
        void StoreInboundSubscription(quicr::ConnectionHandle connection_handle,
                                      uint64_t subscribe_request_id,
                                      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle);

        void ClearInboundSubscriptionsFor(quicr::ConnectionHandle connection_handle);

        void RegisterRelayPublisherBinding(quicr::ConnectionHandle subscriber_connection_handle,
                                           uint64_t subscribe_request_id,
                                           const std::shared_ptr<quicr::PublishTrackHandler>& publish_handler);

        void UnregisterRelayPublisherBinding(quicr::PublishTrackHandler* publish_handler);

        void ClearRelayPublishBindingsForConnection(quicr::ConnectionHandle subscriber_connection_handle);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryRelayPublishObject(
          quicr::PublishTrackHandler* handler,
          quicr::TrackMode track_mode,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryRelayForwardPublishedData(
          quicr::PublishTrackHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data);

        void ClearOutboundFetchStateFor(quicr::ConnectionHandle connection_handle);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchPublishObject(
          quicr::PublishFetchHandler* handler,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchForwardPublishedData(
          quicr::PublishFetchHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data);

        /**
         * @brief Forward peer PUBLISH_NAMESPACE to MoQ clients that SUBSCRIBE_NAMESPACE (libquicr
         *        `ResolvePublishNamespace` fanout when publisher connection handle is 0).
         */
        void FanoutPublishNamespaceAnnounces(
          quicr::ConnectionHandle publisher_connection_handle,
          const quicr::TrackNamespace& track_namespace,
          quicr::messages::RequestID publisher_request_id,
          const std::vector<quicr::ConnectionHandle>& subscriber_connections);

        /** @brief PUBLISH_NAMESPACE_DONE fanout to the same subscribers (`ResolvePublishNamespaceDone`). */
        void FanoutPublishNamespaceDones(
          quicr::messages::RequestID publisher_request_id,
          const std::vector<quicr::ConnectionHandle>& subscriber_connections);

        void ClearPeerFanoutPublishNamespaceHandlesForSubscriber(quicr::ConnectionHandle subscriber_connection_handle);

      private:
        using UpstreamSubKey = std::pair<quicr::ConnectionHandle, uint64_t>;
        using InboundSubKey = std::pair<quicr::ConnectionHandle, uint64_t>;

        std::shared_ptr<moxygen::MoQSession> LockMoqSession(quicr::ConnectionHandle connection_handle) const;

        void StoreUpstreamSubscription(UpstreamSubKey key, std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle);

        std::shared_ptr<moxygen::Publisher::SubscriptionHandle> TakeUpstreamSubscription(const UpstreamSubKey& key);

        std::shared_ptr<moxygen::Publisher::SubscriptionHandle> FindUpstreamSubscription(const UpstreamSubKey& key) const;

        MoxygenClientManager& owner_;
        std::shared_ptr<spdlog::logger> log_;
        mutable std::mutex subs_mu_;
        std::map<UpstreamSubKey, std::shared_ptr<moxygen::Publisher::SubscriptionHandle>> upstream_subs_;

        mutable std::mutex inbound_mu_;
        std::map<InboundSubKey, std::shared_ptr<moxygen::Publisher::SubscriptionHandle>> inbound_subs_;

        struct RelayMoqBinding
        {
            quicr::ConnectionHandle subscriber_conn{ 0 };
            std::weak_ptr<moxygen::MoQSession> session;
            std::shared_ptr<moxygen::Publisher::SubscriptionHandle> subscription_handle;
            std::shared_ptr<moxygen::TrackConsumer> consumer;
            std::mutex consumer_mu;
        };

        mutable std::mutex relay_mu_;
        std::map<quicr::PublishTrackHandler*, std::shared_ptr<RelayMoqBinding>> relay_bindings_;

        mutable std::mutex fetch_state_mu_;
        std::map<std::pair<quicr::ConnectionHandle, uintptr_t>, std::shared_ptr<moxygen::Publisher::FetchHandle>>
          active_outbound_fetches_;
        std::map<std::pair<quicr::ConnectionHandle, uint64_t>, std::weak_ptr<quicr::PublishFetchHandler>>
          fetch_publish_bindings_;
        std::atomic<uint64_t> next_moq_fetch_data_ctx_{ 1 };

        struct MoqFetchResponseStreamState
        {
            quicr::ConnectionHandle connection_handle{};
            proxygen::WebTransport::StreamWriteHandle* write_handle{ nullptr };
            bool publish_fetch_header_sent{ false };
        };

        std::map<quicr::PublishFetchHandler*, MoqFetchResponseStreamState> fetch_response_streams_;

        void FinAndClearMoqFetchStream(MoqFetchResponseStreamState& st);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchPublishObjectOnExecutor(
          quicr::PublishFetchHandler* handler,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data);

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchForwardPublishedDataOnExecutor(
          quicr::PublishFetchHandler* handler,
          bool is_new_stream,
          std::shared_ptr<const std::vector<uint8_t>> data);

        /** @pre fetch_state_mu_ held */
        void RemoveFetchResponseStreamLocked(quicr::PublishFetchHandler* handler);
        /** @pre fetch_state_mu_ held */
        void ClearFetchResponseStreamsForConnectionLocked(quicr::ConnectionHandle connection_handle);

        static bool EnsureRelayTrackConsumer(const std::shared_ptr<RelayMoqBinding>& binding,
                                             quicr::PublishTrackHandler& handler,
                                             const std::shared_ptr<spdlog::logger>& log);

        using PeerFanoutPublishNamespaceKey = std::pair<quicr::ConnectionHandle, quicr::messages::RequestID>;

        mutable std::mutex peer_fanout_ns_mu_;
        std::map<PeerFanoutPublishNamespaceKey, std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle>>
          peer_fanout_publish_namespace_handles_;
    };

} // namespace laps::moq::shim
