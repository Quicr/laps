// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <quicr/session.h>

#include <cstdint>
#include <memory>

namespace laps {
    class ClientManager;

    /**
     * @brief MoQ session for a single client connection
     *
     * @details libquicr creates one Session per connection. Relay state, however, spans all connections, so
     *      this class only adapts the per-connection callbacks into ClientManager calls that carry the
     *      connection handle.
     */
    class ClientSession : public quicr::Session
    {
      public:
        static std::shared_ptr<ClientSession> Create(const quicr::ServerConfig& cfg,
                                                     std::shared_ptr<quicr::Transport> transport,
                                                     std::shared_ptr<quicr::Connection> connection,
                                                     std::shared_ptr<timeq::tick_service> tick_service,
                                                     ClientManager& manager)
        {
            return std::shared_ptr<ClientSession>(
              new ClientSession(cfg, std::move(transport), std::move(connection), std::move(tick_service), manager));
        }

        /**
         * @brief Connection handle that identifies this session within the relay state
         */
        std::uint64_t GetConnectionHandle() const noexcept
        {
            const auto& connection = GetConnection();
            return connection != nullptr ? connection->GetID() : 0;
        }

        // -------------------------------------------------------------------------------
        // quicr::Session callbacks, all forwarded to ClientManager with the connection handle
        // -------------------------------------------------------------------------------

        /**
         * @brief Report the connection going away to the relay
         *
         * @details This is the preferred close signal, since it arrives on the callback thread alongside
         *      every other session callback. It is not reliable on its own: the session manager detaches this
         *      session from the connection when the transport reports the close, which discards any status
         *      notification still queued for the callback thread. The relay's OnSessionRemoved callback
         *      therefore reports the close as well, and ConnectionClosed() tolerates either order.
         */
        void StatusChanged(Status status) override;

        void ClientSetupReceived(const quicr::ClientSetupAttributes& client_setup_attributes) override;

        void SubscribeTracksReceived(std::uint64_t data_ctx_id,
                                     const quicr::TrackNamespace& prefix_namespace,
                                     const quicr::SubscribeNamespaceAttributes& attributes) override;

        void UnsubscribeNamespaceReceived(const quicr::TrackNamespace& prefix_namespace) override;

        void PublishNamespaceReceived(const quicr::TrackNamespace& track_namespace,
                                      const quicr::PublishNamespaceAttributes& attributes) override;

        std::vector<std::uint64_t> PublishNamespaceDoneReceived(std::uint64_t request_id) override;

        void PublishReceived(std::uint64_t request_id,
                             const quicr::PublishAttributes& publish_attributes,
                             std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler) override;

        void PublishDoneReceived(std::uint64_t request_id) override;

        void SubscribeReceived(std::uint64_t request_id,
                               const quicr::FullTrackName& track_full_name,
                               const quicr::SubscribeAttributes& attributes) override;

        void UnsubscribeReceived(std::uint64_t request_id) override;

        void NewGroupRequested(const quicr::FullTrackName& track_full_name, std::uint64_t group_id) override;

        void TrackStatusReceived(std::uint64_t request_id, const quicr::FullTrackName& track_full_name) override;

        void StandaloneFetchReceived(std::uint64_t request_id,
                                     const quicr::FullTrackName& track_full_name,
                                     const quicr::StandaloneFetchAttributes& attributes) override;

        void JoiningFetchReceived(std::uint64_t request_id,
                                  const quicr::FullTrackName& track_full_name,
                                  const quicr::JoiningFetchAttributes& attributes) override;

        void FetchCancelReceived(std::uint64_t request_id) override;

        void MetricsSampled(const quicr::ConnectionMetrics& metrics) override;

      private:
        ClientSession(const quicr::ServerConfig& cfg,
                      std::shared_ptr<quicr::Transport> transport,
                      std::shared_ptr<quicr::Connection> connection,
                      std::shared_ptr<timeq::tick_service> tick_service,
                      ClientManager& manager)
          : quicr::Session(cfg, std::move(transport), std::move(connection), std::move(tick_service))
          , manager_(manager)
        {
        }

        ClientManager& manager_;

        /// Set once the relay has been told the connection is gone, since status changes can repeat
        bool close_reported_{ false };
    };
} // namespace laps
