#pragma once

#include "client_manager.h"
#include "moq_session_handle_registry.h"
#include "moxygen_moq_server_port.h"
#include "relay_core.h"
#include "moxygen_relay_with_policy.h"

#include "moq/shim/laps_moq_relay_session.h"
#include <moxygen/MoQTypes.h>
#include <moxygen/openmoq/transport/pico/MoQPicoQuicServer.h>

#include <quicr/detail/ctrl_message_types.h>

#include <memory>
#include <optional>

namespace laps {
    class MoxygenClientManager
      : public ClientManager
      , public moxygen::MoQPicoQuicServer
    {
      public:
        MoxygenClientManager(State& state, const Config& config, peering::PeerManager& peer_manager);

        ~MoxygenClientManager() override;

        bool Start() override;

        void ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                              uint64_t request_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::messages::SubscribeAttributes& attrs,
                              std::optional<quicr::messages::Location> largest) override;

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

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenRelayPublishObject(
          quicr::PublishTrackHandler* handler,
          quicr::TrackMode track_mode,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data) override;

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenRelayForwardPublishedData(
          quicr::PublishTrackHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data) override;

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchPublishObject(
          quicr::PublishFetchHandler* handler,
          const quicr::ObjectHeaders& object_headers,
          quicr::BytesSpan data) override;

        std::optional<quicr::PublishTrackHandler::PublishObjectStatus> TryMoxygenFetchForwardPublishedData(
          quicr::PublishFetchHandler* handler,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data) override;

        void onNewSession(std::shared_ptr<moxygen::MoQSession> client_session) override;

        moq::shim::MoqSessionHandleRegistry<moxygen::MoQSession>& GetMoqSessionRegistry() noexcept
        {
            return moq_session_registry_;
        }

        moq::shim::RelayCore& GetRelayCore() noexcept { return relay_core_; }

        moq::shim::AnnouncerSubscribeHandlerFactory GetAnnouncerSubscribeHandlerFactory();

        /** MoQ ingress PUBLISH: update laps relay state to match `QuicrClientManager::PublishReceived` (no `ResolvePublish`). */
        void ProcessMoqIngressPublish(quicr::ConnectionHandle connection_handle, const moxygen::PublishRequest& pub);

      protected:
        std::shared_ptr<moxygen::MoQSession> createSession(folly::MaybeManagedPtr<proxygen::WebTransport> wt,
                                                           std::shared_ptr<moxygen::MoQExecutor> executor) override;

      private:
        friend class MoxygenRelayWithPolicy;
        class MoqSessionCloseHook;

        void OnInboundMoqSubscribeOk(const moxygen::SubscribeRequest& sub_req,
                                     std::optional<quicr::messages::Location> largest_from_subscribe_ok);

        moq::shim::MoqSessionHandleRegistry<moxygen::MoQSession> moq_session_registry_;
        std::shared_ptr<moq::shim::MoxygenMoqServerPort> moxygen_moq_port_;
        moq::shim::RelayCore relay_core_;
        const Config& config_;
        std::shared_ptr<MoxygenRelayWithPolicy> relay_{ std::make_shared<MoxygenRelayWithPolicy>(*this, 100, 3) };
    };
}