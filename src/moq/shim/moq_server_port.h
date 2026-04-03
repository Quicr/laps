#pragma once

#include <quicr/common.h>

#include <memory>

namespace quicr {
    class SubscribeTrackHandler;
    class FetchTrackHandler;
    class PublishFetchHandler;
    class PublishNamespaceHandler;
} // namespace quicr

namespace laps::moq::shim {

    /**
     * @brief Outbound transport port — Picoquic path (`QuicrMoqServerPort` → `quicr::Server`) and
     *        future moxygen adapter. Names mirror `quicr::Server` / `quicr::Transport` (PascalCase).
     */
    class MoqServerPort
    {
      public:
        virtual ~MoqServerPort() = default;

        virtual void SubscribeTrack(quicr::ConnectionHandle connection_handle,
                                    std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) = 0;

        virtual void UnsubscribeTrack(quicr::ConnectionHandle connection_handle,
                                      const std::shared_ptr<quicr::SubscribeTrackHandler>& track_handler) = 0;

        virtual void UpdateTrackSubscription(quicr::ConnectionHandle connection_handle,
                                             std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) = 0;

        virtual void PublishNamespace(quicr::ConnectionHandle connection_handle,
                                      std::shared_ptr<quicr::PublishNamespaceHandler> ns_handler,
                                      bool passive = false) = 0;

        virtual void BindFetchTrack(quicr::ConnectionHandle connection_handle,
                                    std::shared_ptr<quicr::PublishFetchHandler> track_handler) = 0;

        virtual void UnbindFetchTrack(quicr::ConnectionHandle connection_handle,
                                      const std::shared_ptr<quicr::PublishFetchHandler>& track_handler) = 0;

        virtual void FetchTrack(quicr::ConnectionHandle connection_handle,
                                std::shared_ptr<quicr::FetchTrackHandler> track_handler) = 0;

        virtual void CancelFetchTrack(quicr::ConnectionHandle connection_handle,
                                      std::shared_ptr<quicr::FetchTrackHandler> track_handler) = 0;
    };

} // namespace laps::moq::shim
