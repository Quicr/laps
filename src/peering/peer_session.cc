// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#include "peer_session.h"
#include "common.h"
#include "peer_manager.h"
#include "peering/messages/connect.h"
#include "peering/messages/connect_response.h"
#include "peering/messages/node_info.h"
#include "peering/messages/subscribe_info.h"

#include <iomanip>
#include <sstream>

namespace laps::peering {

    PeerSession::PeerSession(bool is_inbound,
                             const Config& cfg,
                             const NodeInfo& node_info,
                             const quicr::TransportRemote& remote,
                             PeerManager& manager)
      : peer_config_(remote)
      , config_(cfg)
      , node_info_(node_info)
      , manager_(manager)
      , is_inbound_(is_inbound)
    {
        if (config_.tls_cert_filename_.length() == 0) {
            transport_config_.tls_cert_filename = "";
            transport_config_.tls_key_filename = "";
        }

        controL_msg_buffer_.reserve(kControlMessageBufferSize);

        SPDLOG_LOGGER_DEBUG(LOGGER, "Starting peer session");
    }

    PeerSession::~PeerSession()
    {
        if (not is_inbound_) {
            connection_ = nullptr;
            transport_ = nullptr;
        }

        SPDLOG_LOGGER_DEBUG(LOGGER, "Removing peer session with {0}:{1}", peer_config_.host_or_ip, peer_config_.port);
    }

    PeerSession::StatusValue PeerSession::Status()
    {
        return status_;
    }

    void PeerSession::Connect()
    {
        status_ = StatusValue::kConnecting;
        remote_node_info_ = {};

        if (is_inbound_) {
            status_ = StatusValue::kConnected;
            return;
        }

        connection_ = nullptr;
        transport_ = nullptr;
        control_stream_ = nullptr;
        control_stream_id_.reset();

        peer_sns_.clear();
        rx_stream_headers_.clear();

        transport_ = quicr::Transport::MakeClientTransport(
          peer_config_, transport_config_, config_.tick_service_, config_.quicr_logger_);

        connection_ = transport_->Start();
        if (!connection_) {
            SPDLOG_LOGGER_ERROR(LOGGER, "Failed to connect to peer {}:{}", peer_config_.host_or_ip, peer_config_.port);
            status_ = StatusValue::kDisconnected;
            return;
        }

        connection_->SetDelegate(shared_from_this());

        // The peer answers on the same stream, so the control exchange needs a bidirectional one
        control_stream_ = transport_->CreateRequestStream(connection_);
        control_stream_id_.reset();

        SPDLOG_LOGGER_DEBUG(LOGGER, "Created control stream for peer session");
    }

    SubscribeNodeSetId PeerSession::NextSnsId()
    {
        // Zero means unset, so it is skipped when the counter wraps
        if (last_sns_id_ >= kMaxSnsId) {
            last_sns_id_ = 0;
        }

        return ++last_sns_id_;
    }

    std::pair<SubscribeNodeSetId, bool> PeerSession::AddPeerSnsSourceNode(PeerSessionId in_peer_session_id,
                                                                          SubscribeNodeSetId in_sns_id,
                                                                          NodeIdValueType sub_node_id,
                                                                          uint8_t priority)
    {
        auto [it, new_ingress] = peer_sns_.try_emplace({ in_peer_session_id, in_sns_id });
        auto& sns = it->second;

        if (it->second.id == 0) {
            it->second.id = NextSnsId();
            it->second.priority = priority;
        }

        auto [__, is_new] = sns.nodes.emplace(sub_node_id);

        if (is_new) {
            SendSns(sns, false);
        }

        return { it->second.id, is_new };
    }

    std::pair<SubscribeNodeSetId, bool> PeerSession::AddSubscribeSourceNode(std::uint64_t full_name_hash,
                                                                            NodeIdValueType sub_node_id,
                                                                            uint8_t priority)
    {
        auto [it, _] = sub_sns_.try_emplace(full_name_hash);
        auto& sns = it->second;

        if (it->second.id == 0) {
            it->second.id = NextSnsId();
            it->second.priority = priority;
        }

        auto [__, is_new] = sns.nodes.emplace(sub_node_id);

        if (is_new) {
            SendSns(sns, false);
        }

        return { it->second.id, is_new };
    }

    std::pair<bool, bool> PeerSession::RemoveSubscribeSourceNode(std::uint64_t full_name_hash,
                                                                 NodeIdValueType sub_node_id)
    {
        bool node_removed{ false };
        bool sns_removed{ false };

        auto it = sub_sns_.find(full_name_hash);
        if (it != sub_sns_.end()) {
            auto& sns = it->second;

            node_removed = sns.nodes.erase(sub_node_id) ? true : false;

            if (sns.nodes.empty()) {
                sns_removed = true;

                SendSns(sns, true);

                sub_sns_.erase(it);
            }
        }

        return { node_removed, sns_removed };
    }

    std::pair<bool, bool> PeerSession::RemovePeerSnsSourceNode(PeerSessionId in_peer_session_id,
                                                               SubscribeNodeSetId in_sns_id,
                                                               NodeIdValueType sub_node_id)
    {
        bool node_removed{ false };
        bool sns_removed{ false };

        auto it = peer_sns_.find({ in_peer_session_id, in_sns_id });
        if (it != peer_sns_.end()) {
            auto& sns = it->second;

            if (sub_node_id == 0) { // Remove all
                SendSns(sns, true);
                peer_sns_.erase(it);
            }

            else { // Remove one
                node_removed = sns.nodes.erase(sub_node_id) ? true : false;

                if (sns.nodes.empty()) {
                    sns_removed = true;

                    SendSns(sns, true);

                    peer_sns_.erase(it);
                }
            }
        }

        return { node_removed, sns_removed };
    }

    std::shared_ptr<quicr::Stream> PeerSession::CreateStream(uint8_t priority) const
    {
        if (!IsUsable()) {
            return nullptr;
        }

        return transport_->CreateDataStream(connection_, priority);
    }

    void PeerSession::CloseStream(const std::shared_ptr<quicr::Stream>& stream, quicr::StreamClosedFlag flag)
    {
        if (!IsUsable() || stream == nullptr) {
            return;
        }

        transport_->CloseStream(connection_, stream, flag == quicr::StreamClosedFlag::kReset);
    }

    void PeerSession::SendData(uint8_t priority,
                               uint32_t ttl,
                               const std::shared_ptr<quicr::Stream>& stream,
                               const quicr::Transport::EnqueueFlags& eflags,
                               std::shared_ptr<const std::vector<uint8_t>> data)
    {
        if (status_ != StatusValue::kConnected || !IsUsable())
            return;

        if (!eflags.use_reliable) {
            transport_->EnqueueDatagram(connection_, std::move(data), priority, ttl);
            return;
        }

        if (stream == nullptr) {
            return;
        }

        transport_->Enqueue(connection_, stream, std::move(data), priority, ttl, eflags);
    }

    void PeerSession::SendControl(std::vector<uint8_t> msg) const
    {
        if (!IsUsable() || control_stream_ == nullptr) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "No control stream for peer conn_id {}, dropping control message", GetSessionId());
            return;
        }

        transport_->Enqueue(
          connection_, control_stream_, std::make_shared<std::vector<uint8_t>>(std::move(msg)), 0, 1000);
    }

    void PeerSession::SendSns(const SubscribeNodeSet& sns, bool withdraw) const
    {
        if (status_ != StatusValue::kConnected || !IsUsable())
            return;

        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending SNS id: {} set size: {} withdraw: {}", sns.id, sns.nodes.size(), withdraw);

        SendControl(sns.Serialize(true, withdraw));
    }

    void PeerSession::SendAnnounceInfo(const AnnounceInfo& announce_info, bool withdraw)
    {
        if (status_ != StatusValue::kConnected || !IsUsable())
            return;
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Sending announce info id: {} source_node_id: {} withdraw: {}",
                            announce_info.fullname_hash,
                            announce_info.source_node_id,
                            withdraw);

        SendControl(announce_info.Serialize(true, withdraw));
    }

    void PeerSession::SendSubscribeInfo(SubscribeInfo& subscribe_info, bool withdraw) const
    {
        if (status_ != StatusValue::kConnected || !IsUsable())
            return;
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Sending subscribe fullname: {} source_node_id: {} withdraw: {} sub_data_size: {}",
                            subscribe_info.track_hash.track_fullname_hash,
                            NodeId().Value(subscribe_info.source_node_id),
                            withdraw,
                            subscribe_info.subscribe_data.size());

        SendControl(subscribe_info.Serialize(true, withdraw, node_info_.id == subscribe_info.source_node_id));
    }

    void PeerSession::SendNodeInfo(const NodeInfo& node_info, bool withdraw) const
    {
        if (status_ != StatusValue::kConnected || !IsUsable())
            return;
        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending node info id: {}", NodeId().Value(node_info.id));

        SendControl(node_info.Serialize(true, withdraw));
    }

    void PeerSession::SendConnect()
    {
        peering::Connect connect;
        connect.mode = PeerMode::kBoth;
        connect.node_info = node_info_;

        peer_sns_.clear();

        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending connect length: {}", connect.Serialize().size());

        SendControl(connect.Serialize());
    }

    void PeerSession::SendConnectOk() const
    {
        ConnectResponse connect_resp;
        connect_resp.error = ProtocolError::kNoError;
        connect_resp.node_info = node_info_;
        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending connect ok length: {}", connect_resp.Serialize().size());

        SendControl(connect_resp.Serialize());
    }

    /*
     * Delegate Implementations
     */
    void PeerSession::OnConnectionStatus(const quicr::Connection::Status status)
    {
        const auto conn_id = GetSessionId();

        switch (status) {
            case quicr::Connection::Status::kReady: {
                /*
                 * Inbound sessions do not initiate the peering handshake; they wait for the remote connect
                 * message and answer it with connect ok.
                 */
                if (is_inbound_) {
                    status_ = StatusValue::kConnected;
                    SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is ready", conn_id);
                    break;
                }

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is ready, sending connect message", conn_id);

                SendConnect();
                break;
            }
            case quicr::Connection::Status::kConnecting:
                break;

            case quicr::Connection::Status::kDisconnected: {
                status_ = StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is disconnected", conn_id);
                break;
            }

            case quicr::Connection::Status::kRemoteRequestClose:
                status_ = StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} remote disconnected", conn_id);
                break;

            case quicr::Connection::Status::kShutdown:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;

            case quicr::Connection::Status::kIdleTimeout:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} idle timeout", conn_id);
                break;

            case quicr::Connection::Status::kShuttingDown:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;
        }

        manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);
    }

    void PeerSession::ProcessControlMessage()
    try {
        if (controL_msg_buffer_.size() >= kCommonHeadersSize) {
            auto cursor_it = controL_msg_buffer_.begin();
            auto bytes = std::span<uint8_t>{ cursor_it, cursor_it + kCommonHeadersSize };
            cursor_it += kCommonHeadersSize;

            // TODO(tievens): Implement version checking and error handling
            auto version = bytes.front();
            auto type = ValueOf<uint16_t>({ bytes.begin() + 1, bytes.begin() + 3 });
            auto data_len = ValueOf<uint32_t>({ bytes.begin() + 3, bytes.begin() + 7 });

            if (controL_msg_buffer_.size() >= kCommonHeadersSize + data_len) {
                auto msg_bytes = std::span{ cursor_it, cursor_it + data_len };

                // Control Message
                switch (static_cast<MsgType>(type)) {
                    case MsgType::kConnect: {
                        peering::Connect connect(msg_bytes);
                        SPDLOG_LOGGER_DEBUG(config_.logger_,
                                            "Connect from id: {} contact: {} mode: {} version: {}",
                                            NodeId().Value(connect.node_info.id),
                                            connect.node_info.contact,
                                            static_cast<int>(connect.mode),
                                            static_cast<int>(version));
                        remote_node_info_ = connect.node_info;

                        status_ = StatusValue::kConnected;

                        manager_.NodeReceived(GetSessionId(), connect.node_info, false);
                        manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);

                        SendConnectOk();

                        manager_.InfoBaseSyncPeer(*this);
                        break;
                    }

                    case MsgType::kConnectResponse: {
                        ConnectResponse connect_resp(msg_bytes);

                        if (connect_resp.error == ProtocolError::kNoError) {
                            remote_node_info_ = *connect_resp.node_info;
                            manager_.NodeReceived(GetSessionId(), *connect_resp.node_info, false);

                            manager_.InfoBaseSyncPeer(*this);

                        } else {
                            SPDLOG_LOGGER_DEBUG(config_.logger_,
                                                "Connect error response from error: {}",
                                                static_cast<int>(connect_resp.error));
                        }
                        status_ = StatusValue::kConnected;
                        manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);

                        manager_.InfoBaseSyncPeer(*this);
                        break;
                    }

                    case MsgType::kSubscribeNodeSetAdvertised: {
                        SubscribeNodeSet sns(msg_bytes, false);

                        if (config_.debug) {
                            std::ostringstream sns_nodes;
                            for (const auto& node : sns.nodes) {
                                sns_nodes << NodeId().Value(node) << ", ";
                            }

                            SPDLOG_LOGGER_DEBUG(LOGGER, "SNS received id: {} nodes: {}", sns.id, sns_nodes.str());
                        }

                        manager_.SnsReceived(*this, sns, false);
                        break;
                    }

                    case MsgType::kSubscribeNodeSetWithdrawn: {
                        SubscribeNodeSet sns(msg_bytes, true);
                        SPDLOG_LOGGER_DEBUG(LOGGER, "SNS withdrawn received id: {}", sns.id);
                        manager_.SnsReceived(*this, sns, true);
                        break;
                    }

                    case MsgType::kNodeInfoAdvertise: {
                        NodeInfo node_info(msg_bytes);
                        manager_.NodeReceived(GetSessionId(), node_info, false);
                        break;
                    }

                    case MsgType::kNodeInfoWithdrawn: {
                        NodeInfo node_info(msg_bytes);
                        manager_.NodeReceived(GetSessionId(), node_info, true);
                        break;
                    }

                    case MsgType::kSubscribeInfoAdvertised: {
                        SubscribeInfo subscribe_info(msg_bytes);
                        manager_.SubscribeInfoReceived(GetSessionId(), subscribe_info, false);
                        break;
                    }

                    case MsgType::kSubscribeInfoWithdrawn: {
                        SubscribeInfo subscribe_info(msg_bytes);
                        manager_.SubscribeInfoReceived(GetSessionId(), subscribe_info, true);
                        break;
                    }

                    case MsgType::kAnnounceInfoAdvertised: {
                        AnnounceInfo announce_info(msg_bytes);
                        manager_.AnnounceInfoReceived(GetSessionId(), announce_info, false);
                        break;
                    }

                    case MsgType::kAnnounceInfoWithdrawn: {
                        AnnounceInfo announce_info(msg_bytes);
                        manager_.AnnounceInfoReceived(GetSessionId(), announce_info, true);
                        break;
                    }

                    default: {
                        SPDLOG_LOGGER_DEBUG(config_.logger_, "Invalid message type {}", static_cast<int>(type));
                    }
                }

                controL_msg_buffer_.erase(controL_msg_buffer_.begin(), cursor_it + data_len);
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_LOGGER_ERROR(config_.logger_, "Unable to parse control message: {}", e.what());
        controL_msg_buffer_.clear();
    }

    bool PeerSession::ProcessReceivedData(std::uint64_t stream_id, std::shared_ptr<const std::vector<uint8_t>> data)
    {
        // TODO(tievens): Update to not buffer when node type is Via

        constexpr quicr::Transport::EnqueueFlags eflags{ true, false, false, false };

        const auto header_it = rx_stream_headers_.find(stream_id);

        // NEW STREAM - parse start of stream headers
        if (header_it == rx_stream_headers_.end()) {
            if (data->empty() || data->size() < data->front()) {
                SPDLOG_LOGGER_DEBUG(
                  LOGGER,
                  "Received new data object stream id: {}, not enough bytes yet to read headers {} > {}",
                  stream_id,
                  data->empty() ? 0 : data->front(),
                  data->size());
                return false; // Not enough bytes to parse the headers, wait till more arrives
            }

            const auto hdr_len = data->front();

            auto& data_header = rx_stream_headers_[stream_id];
            data_header.Deserialize(*data);

            // Pipeline forward to other peers.
            manager_.ForwardPeerData(GetSessionId(), true, stream_id, data_header, data, hdr_len, eflags);

            return true;
        }

        // Pipeline forward to other peers. Not all data may have been popped, so only forward popped data
        manager_.ForwardPeerData(GetSessionId(), false, stream_id, header_it->second, data, 0, eflags);

        return true;
    }

    void PeerSession::OnRecvStream(std::uint64_t stream_id,
                                   const std::shared_ptr<quicr::StreamRxContext>& rx_ctx,
                                   const std::shared_ptr<quicr::Stream>& stream,
                                   const bool is_bidir)
    {
        if (!IsUsable() || rx_ctx == nullptr) {
            return;
        }

        /*
         * Control messages arrive on the one bidirectional stream, which the inbound side of the session
         * only learns about here. Both sides answer on it, so it is adopted either way.
         */
        if (is_bidir && stream != nullptr) {
            control_stream_ = stream;
            control_stream_id_ = stream_id;
        }

        for (int i = 0; i < kReadLoopMaxPerStream; i++) {
            if (rx_ctx->data_queue.Empty()) {
                break;
            }

            auto data_opt = rx_ctx->data_queue.Pop();
            if (not data_opt.has_value()) {
                break;
            }

            const auto& data = data_opt.value();

            // Get common header
            if (is_bidir) { // control
                controL_msg_buffer_.insert(controL_msg_buffer_.end(), data->begin(), data->end());

                ProcessControlMessage();

            } else if (!ProcessReceivedData(stream_id, std::move(data))) {
                i = 59;
                continue; // Try once more
            }
        }
    }

    void PeerSession::OnRecvDgram()
    {
        constexpr quicr::Transport::EnqueueFlags eflags{ false, false, false, false };

        if (!IsUsable()) {
            return;
        }

        for (int i = 0; i < 80; i++) {
            auto data = transport_->Dequeue(connection_);

            if (!data) {
                return;
            }

            DataHeader data_header(*data);

            manager_.ForwardPeerData(GetSessionId(), false, 0, data_header, data, data_header.header_len, eflags);

            SPDLOG_LOGGER_TRACE(LOGGER,
                                "Received dgram sns_id: {} track_full_name: {} data size: {}",
                                data_object.sns_id,
                                data_object.track_full_name_hash,
                                data_object.data.size());
        }
    }

    void PeerSession::OnConnectionMetricsSampled([[maybe_unused]] const quicr::MetricsTimeStamp sample_time,
                                                 const quicr::QuicConnectionMetrics& quic_connection_metrics)
    {
        metrics_.srtt_us = quic_connection_metrics.srtt_us.avg;
    }

    void PeerSession::OnStreamClosed(std::uint64_t stream_id,
                                     [[maybe_unused]] std::shared_ptr<quicr::StreamRxContext> rx_context,
                                     quicr::StreamClosedFlag flag)
    {
        const auto conn_id = GetSessionId();

        if (control_stream_ != nullptr && control_stream_id_.has_value() && *control_stream_id_ == stream_id) {
            control_stream_ = nullptr;
            control_stream_id_.reset();
        }

        if (const auto header_it = rx_stream_headers_.find(stream_id); header_it != rx_stream_headers_.end()) {
            const auto& data_header = header_it->second;

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Peer stream closed conn_id {} stream id: {} flag: {} track fullname hash: {}",
                                conn_id,
                                stream_id,
                                static_cast<int>(flag),
                                data_header.track_full_name_hash);
            manager_.CloseStream(conn_id, data_header.sns_id, stream_id, data_header.track_full_name_hash, flag);

            rx_stream_headers_.erase(header_it);

        } else {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Peer conn_id {} stream id: {} flag: {} closed without having existing state",
                                conn_id,
                                stream_id,
                                static_cast<int>(flag));
        }
    }

}