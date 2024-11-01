// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <memory>
#include <optional>
#include <quicr/detail/quic_transport.h>

#include "../config.h"
#include "connect.h"
#include "node_info.h"

namespace laps::peering {
    class PeerManager;

    /**
     * @brief Peering manager class. Manages relay to relay (peering) forwarding of
     *      subscriber objects.
     *
     */
    class PeerSession : public quicr::ITransport::TransportDelegate
    {
      public:
        enum class StatusValue : uint8_t
        {
            kConnecting = 0,
            kConnected,
            kDisconnected
        };

        PeerSession& operator=(const PeerSession&) { return *this; }

        PeerSession() = delete;

     // --------------------------------------------------------------------------------------------
        // public methods
        // --------------------------------------------------------------------------------------------

        /**
         * @brief Constructor to create a new peer session
         *
         * @param is_inbound                True indicates the peering session is inbound (server accepted)
         * @param conn_id                   Connection ID from the transport for the connection
         * @param cfg                       Global config
         * @param remote                    Transport remote peer config/parameters
         * @param callbacks                 Peer Manager callbacks
         * @param tick_service              Tick service
         */
        PeerSession(const bool is_inbound,
                    const quicr::TransportConnId conn_id,
                    const Config& cfg,
                    const NodeInfo& node_info,
                    const quicr::TransportRemote& remote,
                    PeerManager& manager);

        ~PeerSession();

        /**
         * @brief Create a connection using the transport to the peer
         */
        void Connect();

        /**
         * @brief Get the status of the peer session
         */
        StatusValue Status();

        /**
         * @brief Set the transport
         * @details Setting the transport is not required and should not be used for outbound connections. This
         *     Server,incoming mode requires the server transport to be used.
         */
        void SetTransport(std::shared_ptr<quicr::ITransport> transport) { transport_ = transport; }

        /**
         * @brief Get the peer session ID
         */
        PeerSessionId GetSessionId() const { return t_conn_id_; }

        void SendNodeInfo(const NodeInfo& node_info);

        /*
         * Delegate functions mainly for Outgoing but does include incoming
         */
        void OnNewDataContext(const quicr::TransportConnId& conn_id, const quicr::DataContextId& data_ctx_id) override
        {
        }
        void OnConnectionStatus(const quicr::TransportConnId& conn_id, const quicr::TransportStatus status) override;
        void OnNewConnection(const quicr::TransportConnId& conn_id, const quicr::TransportRemote& remote) override;
        void OnRecvStream(const quicr::TransportConnId& conn_id,
                          uint64_t stream_id,
                          std::optional<quicr::DataContextId> data_ctx_id,
                          const bool is_bidir = false) override;
        void OnRecvDgram(const quicr::TransportConnId& conn_id,
                         std::optional<quicr::DataContextId> data_ctx_id) override;

        void OnConnectionMetricsSampled(const quicr::MetricsTimeStamp sample_time,
                                        const quicr::TransportConnId conn_id,
                                        const quicr::QuicConnectionMetrics& quic_connection_metrics) override;

        // ---------------------------------------

      private:
        void SendConnect();
        void SendConnectOk();

      public:
        quicr::TransportRemote peer_config_;
        const Config& config_;
        const NodeInfo node_info_;
        NodeInfo remote_node_info_;

        struct Metrics
        {
            uint64_t srtt_us; /// smooth round trip time sampled from the transport, using average value
        } metrics_;

      private:
        PeerManager& manager_;
        StatusValue status_{ StatusValue::kConnecting }; /// Status of this peer session

        bool is_inbound_{ false }; /// Indicates if the peer is server accepted (inbound) or client (outbound)

        quicr::TransportConfig transport_config_{
            .tls_cert_filename = config_.tls_cert_filename_,
            .tls_key_filename = config_.tls_key_filename_,
            .time_queue_init_queue_size = config_.peering.init_queue_size,
            .time_queue_max_duration = config_.peering.max_ttl_expiry_ms,
            .debug = config_.debug,
        };

        quicr::TransportConnId t_conn_id_;         /// Transport connection context ID (aka peer session id)
        quicr::DataContextId control_data_ctx_id_; /// Control data context ID

        std::shared_ptr<quicr::ITransport> transport_; /// Transport used for the peering connection
    };

} // namespace laps