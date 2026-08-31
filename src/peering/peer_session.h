// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <map>
#include <optional>
#include <quicr/connection.h>
#include <quicr/stream.h>
#include <quicr/transport.h>
#include <set>

#include "config.h"
#include "messages/announce_info.h"
#include "messages/data_header.h"
#include "messages/node_info.h"
#include "messages/subscribe_info.h"
#include "messages/subscribe_node_set.h"

namespace laps::peering {

    class PeerManager;

    /**
     * @brief Peering manager class. Manages relay to relay (peering) forwarding of
     *      subscriber objects.
     *
     */
    class PeerSession
      : public quicr::Connection::Delegate
      , public std::enable_shared_from_this<PeerSession>
    {
      public:
        static constexpr std::size_t kControlMessageBufferSize = 4096;

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
         * @param cfg                       Global config
         * @param remote                    Transport remote peer config/parameters
         */
        PeerSession(const bool is_inbound,
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
         * @brief Set the transport and accepted connection
         *
         * @details Only used for inbound (server accepted) sessions, which share the server transport. Outbound
         *      sessions create their own transport and connection in Connect().
         */
        void SetConnection(std::shared_ptr<quicr::Transport> transport, std::shared_ptr<quicr::Connection> connection)
        {
            transport_ = std::move(transport);
            connection_ = std::move(connection);
        }

        /**
         * @brief Get the peer session ID
         */
        PeerSessionId GetSessionId() const { return connection_ ? connection_->GetID() : 0; }

        /**
         * @brief Open an outbound stream to carry objects for a subscribe node set
         *
         * @details The handle is held by the caller for as long as it keeps sending on the stream, rather
         *      than being looked up per object.
         *
         * @returns The stream, or nullptr if the session has no usable connection
         */
        std::shared_ptr<quicr::Stream> CreateStream(uint8_t priority) const;

        void CloseStream(const std::shared_ptr<quicr::Stream>& stream, quicr::StreamClosedFlag flag);
        void SendNodeInfo(const NodeInfo& node_info, bool withdraw = false) const;
        void SendSubscribeInfo(SubscribeInfo& subscribe_info, bool withdraw = false) const;
        void SendAnnounceInfo(const AnnounceInfo& announce_info, bool withdraw = false);
        void SendSns(const SubscribeNodeSet& sns, bool withdraw = false) const;

        /**
         * @brief Send an object to the peer
         *
         * @param stream    Stream to send on, ignored when the flags ask for a datagram
         */
        void SendData(uint8_t priority,
                      uint32_t ttl,
                      const std::shared_ptr<quicr::Stream>& stream,
                      const quicr::Transport::EnqueueFlags& eflags,
                      std::shared_ptr<const std::vector<uint8_t>> data);

        /**
         * @brief Add subscriber source node to the peer SNS state
         *
         * @param in_peer_session_id Ingress peer session ID
         * @param in_sns_id          Ingress peer session SNS ID
         * @param sub_node_id        Source NodeId of the node that has the subscriber
         * @param priority           Priority to use for the data context
         *
         * @returns pair Subscribe Node Set Id and True if subscriber node is new or False if existing
         */
        std::pair<SubscribeNodeSetId, bool> AddPeerSnsSourceNode(PeerSessionId in_peer_session_id,
                                                                 SubscribeNodeSetId in_sns_id,
                                                                 NodeIdValueType sub_node_id,
                                                                 uint8_t priority);

        /**
         * @brief Add subscriber source node to subscriber id state
         *
         * @param full_name_hash     Subscribe ID (aka track alias)
         * @param sub_node_id        Source NodeId of the node that has the subscriber
         * @param priority           Priority to use for the data context
         * @returns pair Subscribe Node Set Id and True if subscriber node is new or False if existing
         */
        std::pair<SubscribeNodeSetId, bool> AddSubscribeSourceNode(std::uint64_t full_name_hash,
                                                                   NodeIdValueType sub_node_id,
                                                                   uint8_t priority);

        /**
         * @brief Remove subscriber source node from the subscribe id state
         *
         * @details Removes the subscribe source node from the nodes set. When there are no
         *   nodes left, the SNS will be removed, resulting in the transport data connection
         *   being closed. The SNS ID will no longer be valid.
         *
         * @param full_name_hash     Subscribe ID (aka track alias)
         * @param sub_node_id      Source NodeId of the node that has the subscriber
         *
         * @eturns First bool indicates true if source node was removed and second indicates true if there are
         *   no subscribe nodes
         */
        std::pair<bool, bool> RemoveSubscribeSourceNode(std::uint64_t full_name_hash, NodeIdValueType sub_node_id);

        /**
         * @brief Remove subscriber source node from the peer SNS state
         *
         * @param in_peer_session_id Ingress peer session ID
         * @param in_sns_id          Ingress peer session SNS ID
         * @param sub_node_id        Source NodeId of the node that has the subscriber
         *
         * @eturns First bool indicates true if source node was removed and second indicates true if there are
         *   no subscribe nodes
         */
        std::pair<bool, bool> RemovePeerSnsSourceNode(PeerSessionId in_peer_session_id,
                                                      SubscribeNodeSetId in_sns_id,
                                                      NodeIdValueType sub_node_id);

        /*
         * Connection delegate callbacks
         */
        void OnConnectionStatus(quicr::Connection::Status status) override;

        void OnRecvStream(std::uint64_t stream_id,
                          const std::shared_ptr<quicr::StreamRxContext>& rx_ctx,
                          const std::shared_ptr<quicr::Stream>& stream,
                          bool is_bidir = false) override;

        void OnRecvDgram() override;

        void OnConnectionMetricsSampled(const quicr::MetricsTimeStamp sample_time,
                                        const quicr::QuicConnectionMetrics& quic_connection_metrics) override;

        void OnStreamMetricsStampled(const quicr::MetricsTimeStamp,
                                     std::uint64_t,
                                     const quicr::QuicStreamMetrics&,
                                     bool) override
        {
        }

        void OnStreamClosed(std::uint64_t stream_id,
                            std::shared_ptr<quicr::StreamRxContext> rx_context,
                            quicr::StreamClosedFlag flag) override;

        // ---------------------------------------

      private:
        /// True when the transport and connection are both present, so transport calls are safe to make
        bool IsUsable() const { return transport_ != nullptr && connection_ != nullptr; }

        void SendConnect();
        void SendConnectOk() const;

        /**
         * @brief Next subscribe node set ID for this session
         *
         * @details SNS IDs identify a flow of objects on the wire within a peering session. They used to be
         *      the transport's data context ID for the flow, but the transport no longer has that concept.
         */
        SubscribeNodeSetId NextSnsId();

        /// Send a control message on the session's bidirectional control stream
        void SendControl(std::vector<uint8_t> msg) const;

        void ProcessControlMessage();

        /**
         * @brief Forward an object received on a stream to the other peers interested in it
         *
         * @returns False when the start of the stream is too short to hold its header, so the caller should
         *      wait for more of it to arrive
         */
        bool ProcessReceivedData(std::uint64_t stream_id, std::shared_ptr<const std::vector<uint8_t>> data);

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
            .time_queue_max_duration = config_.object_ttl_ * 2,
            .debug = config_.debug,
        };

        /// Map of all subscriber source nodes, indexed by subscribe full track name hash (aka track alias)
        std::map<std::uint64_t, SubscribeNodeSet> sub_sns_;

        /// Map of subscriber source nodes initiated by peer ingress SNS.
        /// Key is the ingress peer session ID and SNS ID, value is the SNS egress via this peer
        std::map<std::pair<PeerSessionId, SubscribeNodeSetId>, SubscribeNodeSet> peer_sns_;

        /// Last SNS ID handed out on this session, which is scoped to the session
        SubscribeNodeSetId last_sns_id_{ 0 };

        /**
         * @brief Bidirectional stream carrying the peering control messages
         *
         * @details Opened by the outbound side and learned by the inbound side from the first control
         *      message it receives, so both sides send on the one stream.
         */
        std::shared_ptr<quicr::Stream> control_stream_;

        std::vector<uint8_t> controL_msg_buffer_; /// Working buffer of control message being processed

        /**
         * @brief Header of each inbound object stream, by stream ID
         *
         * @details Parsed from the start of the stream and needed again for the objects that follow it,
         *      which do not repeat it. Only touched from the connection's callback thread.
         */
        std::map<std::uint64_t, DataHeader> rx_stream_headers_;

        std::shared_ptr<quicr::Connection> connection_; /// Connection (aka peer session) to the peer
        std::shared_ptr<quicr::Transport> transport_;   /// Transport used for the peering connection
    };

} // namespace laps