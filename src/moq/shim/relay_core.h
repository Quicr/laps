#pragma once

#include "moq_server_port.h"

#include "config.h"
#include "peering/peer_manager.h"
#include "state.h"

#include <quicr/detail/messages.h>
#include <quicr/track_name.h>

#include <functional>
#include <memory>
#include <optional>

namespace laps {
    class SubscribeTrackHandler;
} // namespace laps

namespace laps::moq::shim {

    /**
     * @brief Builds the laps `SubscribeTrackHandler` used when this relay subscribes upstream to an announcer.
     *        Libquicr supplies a real factory; moxygen may pass `{}` or a factory that returns nullptr until bridging
     *        can construct a handler backed by `MoxygenClientManager` / `MoQSession`.
     */
    using AnnouncerSubscribeHandlerFactory =
      std::function<std::shared_ptr<laps::SubscribeTrackHandler>(const quicr::FullTrackName& full_track_name,
                                                                 quicr::messages::ObjectPriority priority,
                                                                 quicr::messages::GroupOrder group_order)>;

    /**
     * @brief Shared relay policy (P2); `QuicrClientManager` delegates subscribe/dampen paths here.
     *        Outbound I/O goes through `MoqServerPort` for backend swap (libquicr vs moxygen).
     */
    class RelayCore
    {
      public:
        RelayCore(State& state,
                  const Config& config,
                  peering::PeerManager& peer_manager,
                  std::shared_ptr<MoqServerPort> port)
          : state_(state)
          , config_(config)
          , peer_manager_(peer_manager)
          , port_(std::move(port))
        {
        }

        void SetPort(std::shared_ptr<MoqServerPort> port) { port_ = std::move(port); }

        const std::shared_ptr<MoqServerPort>& GetPort() const noexcept { return port_; }

        State& GetState() { return state_; }
        const Config& GetConfig() const { return config_; }
        peering::PeerManager& GetPeerManager() { return peer_manager_; }

        void ProcessSubscribe(const AnnouncerSubscribeHandlerFactory& announcer_subscribe_handler_factory,
                              quicr::ConnectionHandle connection_handle,
                              uint64_t request_id,
                              const quicr::TrackHash& th,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::messages::SubscribeAttributes& attrs,
                              std::optional<quicr::messages::Location> largest);

        bool DampenOrUpdateTrackSubscription(std::shared_ptr<laps::SubscribeTrackHandler> sub_to_pub_track_handler,
                                             bool new_group_request);

      private:
        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;
        std::shared_ptr<MoqServerPort> port_;
    };

} // namespace laps::moq::shim
