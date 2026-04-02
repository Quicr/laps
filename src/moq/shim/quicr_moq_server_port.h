#pragma once

#include "moq_server_port.h"

#include <quicr/server.h>

namespace laps::moq::shim {

    /**
     * @brief Forwards MoqServerPort calls to an existing `quicr::Server` (typically `QuicrClientManager`).
     */
    class QuicrMoqServerPort final : public MoqServerPort
    {
      public:
        explicit QuicrMoqServerPort(quicr::Server& server)
          : server_(server)
        {
        }

        void SubscribeTrack(quicr::ConnectionHandle connection_handle,
                            std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) override
        {
            server_.SubscribeTrack(connection_handle, std::move(track_handler));
        }

        void UnsubscribeTrack(quicr::ConnectionHandle connection_handle,
                              const std::shared_ptr<quicr::SubscribeTrackHandler>& track_handler) override
        {
            server_.UnsubscribeTrack(connection_handle, track_handler);
        }

        void UpdateTrackSubscription(quicr::ConnectionHandle connection_handle,
                                     std::shared_ptr<quicr::SubscribeTrackHandler> track_handler) override
        {
            server_.UpdateTrackSubscription(connection_handle, std::move(track_handler));
        }

        void PublishNamespace(quicr::ConnectionHandle connection_handle,
                              std::shared_ptr<quicr::PublishNamespaceHandler> ns_handler,
                              bool passive = false) override
        {
            server_.PublishNamespace(connection_handle, std::move(ns_handler), passive);
        }

        void BindFetchTrack(quicr::ConnectionHandle connection_handle,
                            std::shared_ptr<quicr::PublishFetchHandler> track_handler) override
        {
            server_.BindFetchTrack(connection_handle, std::move(track_handler));
        }

        void UnbindFetchTrack(quicr::ConnectionHandle connection_handle,
                              const std::shared_ptr<quicr::PublishFetchHandler>& track_handler) override
        {
            server_.UnbindFetchTrack(connection_handle, track_handler);
        }

        void FetchTrack(quicr::ConnectionHandle connection_handle,
                        std::shared_ptr<quicr::FetchTrackHandler> track_handler) override
        {
            server_.FetchTrack(connection_handle, std::move(track_handler));
        }

        void CancelFetchTrack(quicr::ConnectionHandle connection_handle,
                              std::shared_ptr<quicr::FetchTrackHandler> track_handler) override
        {
            server_.CancelFetchTrack(connection_handle, std::move(track_handler));
        }

      private:
        quicr::Server& server_;
    };

} // namespace laps::moq::shim
