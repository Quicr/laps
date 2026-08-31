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
     * @details Request callbacks are delivered through quicr::Session::ServerCallbacks, which the relay
     *      implements once for all connections. Sampled connection metrics are the exception: libquicr reports
     *      them by calling the session itself, so the relay creates its sessions as this subclass to receive
     *      them.
     */
    class ClientSession : public quicr::Session
    {
      public:
        static std::shared_ptr<ClientSession> Create(const quicr::ServerConfig& cfg,
                                                     std::shared_ptr<quicr::Transport> transport,
                                                     std::shared_ptr<quicr::Connection> connection,
                                                     std::shared_ptr<quicr::Session::ServerCallbacks> callbacks,
                                                     std::shared_ptr<timeq::tick_service> tick_service,
                                                     std::shared_ptr<quicr::Logger> logger,
                                                     ClientManager& manager)
        {
            return std::shared_ptr<ClientSession>(new ClientSession(cfg,
                                                                    std::move(transport),
                                                                    std::move(connection),
                                                                    std::move(callbacks),
                                                                    std::move(tick_service),
                                                                    std::move(logger),
                                                                    manager));
        }

        /**
         * @brief Connection handle that identifies this session within the relay state
         */
        std::uint64_t GetConnectionHandle() const noexcept
        {
            const auto& connection = GetConnection();
            return connection != nullptr ? connection->GetID() : 0;
        }

        void MetricsSampled(const quicr::ConnectionMetrics& metrics) override;

      private:
        ClientSession(const quicr::ServerConfig& cfg,
                      std::shared_ptr<quicr::Transport> transport,
                      std::shared_ptr<quicr::Connection> connection,
                      std::shared_ptr<quicr::Session::ServerCallbacks> callbacks,
                      std::shared_ptr<timeq::tick_service> tick_service,
                      std::shared_ptr<quicr::Logger> logger,
                      ClientManager& manager)
          : quicr::Session(cfg,
                           std::move(transport),
                           std::move(connection),
                           std::move(callbacks),
                           std::move(tick_service),
                           std::move(logger))
          , manager_(manager)
        {
        }

        ClientManager& manager_;
    };
} // namespace laps
