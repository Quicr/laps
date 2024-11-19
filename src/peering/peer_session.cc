// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#include "peer_session.h"
#include "common.h"
#include "peer_manager.h"
#include "peering/messages/connect.h"
#include "peering/messages/connect_response.h"
#include "peering/messages/node_info.h"
#include "peering/messages/subscribe_info.h"

#include <sstream>

namespace laps::peering {

    PeerSession::PeerSession(bool is_inbound,
                             const quicr::TransportConnId conn_id,
                             const Config& cfg,
                             const NodeInfo& node_info,
                             const quicr::TransportRemote& remote,
                             PeerManager& manager)
      : peer_config_(remote)
      , config_(cfg)
      , node_info_(node_info)
      , manager_(manager)
      , is_inbound_(is_inbound)
      , t_conn_id_(conn_id)
    {
        if (config_.tls_cert_filename_.length() == 0) {
            transport_config_.tls_cert_filename = "";
            transport_config_.tls_key_filename = "";
        }

        SPDLOG_LOGGER_DEBUG(LOGGER, "Starting peer session");
    }

    PeerSession::~PeerSession()
    {
        if (not is_inbound_) {
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

        if (transport_)
            transport_ = nullptr;

        transport_ = quicr::ITransport::MakeClientTransport(
          peer_config_, transport_config_, *this, manager_.tick_service_, LOGGER);
        t_conn_id_ = transport_->Start();

        // Create the control data context
        control_data_ctx_id_ = transport_->CreateDataContext(t_conn_id_, true, 0, true);

        SPDLOG_LOGGER_DEBUG(LOGGER, "Control stream ID {0}", control_data_ctx_id_);
    }

    std::pair<SubscribeNodeSetId, bool> PeerSession::AddPeerSnsSourceNode(PeerSessionId in_peer_session_id,
                                                                          SubscribeNodeSetId in_sns_id,
                                                                          NodeIdValueType sub_node_id)
    {
        auto [it, new_ingress] = peer_sns_.try_emplace({in_peer_session_id, in_sns_id});
        auto& sns = it->second;

        if (it->second.id == 0) { // If not set, create the data context
            // TODO(tievens): Add datagram support - update transport to allow changing reliable state
            // TODO(tievens): Update transport to have max data context ID and to wrap if reaching max
            it->second.id = transport_->CreateDataContext(t_conn_id_, true, 2, false);
        }

        auto [__, is_new] = sns.nodes.emplace(sub_node_id);

        if (is_new) {
            SendSns(sns, false);
        }

        return { it->second.id, is_new };
    }


    std::pair<SubscribeNodeSetId, bool> PeerSession::AddSubscribeSourceNode(quicr::TrackFullNameHash full_name_hash,
                                                                            NodeIdValueType sub_node_id)
    {
        auto [it, _] = sub_sns_.try_emplace(full_name_hash);
        auto& sns = it->second;

        if (it->second.id == 0) { // If not set, create the data context
            // TODO(tievens): Add datagram support - update transport to allow changing reliable state
            // TODO(tievens): Update transport to have max data context ID and to wrap if reaching max
            it->second.id = transport_->CreateDataContext(t_conn_id_, true, 2, false);
        }

        auto [__, is_new] = sns.nodes.emplace(sub_node_id);

        if (is_new) {
            SendSns(sns, false);
        }

        return { it->second.id, is_new };
    }

    std::pair<bool, bool> PeerSession::RemoveSubscribeSourceNode(quicr::TrackFullNameHash full_name_hash,
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
                transport_->DeleteDataContext(t_conn_id_, it->second.id);

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

        auto it = peer_sns_.find({in_peer_session_id, in_sns_id});
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
                    transport_->DeleteDataContext(t_conn_id_, it->second.id);

                    SendSns(sns, true);

                    peer_sns_.erase(it);
                }
            }
        }

        return { node_removed, sns_removed };
    }

    void PeerSession::SendSns(const SubscribeNodeSet& sns, bool withdraw)
    {
        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending SNS id: {} set size: {} withdraw: {}", sns.id, sns.nodes.size(), withdraw);

        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, sns.Serialize(true, withdraw), 0, 1000);
    }


    void PeerSession::SendAnnounceInfo(const AnnounceInfo& announce_info, bool withdraw)
    {
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Sending announce info id: {} source_node_id: {} withdraw: {}",
                            announce_info.full_name.full_name_hash,
                            announce_info.source_node_id,
                            withdraw);
        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, announce_info.Serialize(true, withdraw), 0, 1000);
    }

    void PeerSession::SendSubscribeInfo(const SubscribeInfo& subscribe_info, bool withdraw)
    {
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Sending subscribe fullname: {} source_node_id: {} withdraw: {}",
                            subscribe_info.track_hash.track_fullname_hash,
                            subscribe_info.source_node_id,
                            withdraw);
        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, subscribe_info.Serialize(true, withdraw), 0, 1000);
    }

    void PeerSession::SendNodeInfo(const NodeInfo& node_info, bool withdraw)
    {
        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending node info id: {}", node_info.id);
        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, node_info.Serialize(true, withdraw), 0, 1000);
    }

    void PeerSession::SendConnect()
    {
        peering::Connect connect;
        connect.mode = PeerMode::kBoth;
        connect.node_info = node_info_;

        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending connect length: {}", connect.Serialize().size());
        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, connect.Serialize(), 0, 1000);
    }

    void PeerSession::SendConnectOk()
    {
        ConnectResponse connect_resp;
        connect_resp.error = ProtocolError::kNoError;
        connect_resp.node_info = node_info_;
        SPDLOG_LOGGER_DEBUG(LOGGER, "Sending connect ok length: {}", connect_resp.Serialize().size());
        transport_->Enqueue(t_conn_id_, control_data_ctx_id_, connect_resp.Serialize(), 0, 1000);
    }

    /*
     * Delegate Implementations
     */
    void PeerSession::OnConnectionStatus(const quicr::TransportConnId& conn_id, const quicr::TransportStatus status)
    {
        switch (status) {
            case quicr::TransportStatus::kReady: {
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is ready, sending connect message", conn_id);

                SendConnect();
                break;
            }
            case quicr::TransportStatus::kConnecting:
                break;

            case quicr::TransportStatus::kDisconnected: {
                status_ = StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is disconnected", conn_id);
                break;
            }

            case quicr::TransportStatus::kRemoteRequestClose:
                status_ = StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} remote disconnected", conn_id);
                break;

            case quicr::TransportStatus::kShutdown:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;

            case quicr::TransportStatus::kIdleTimeout:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} idle timeout", conn_id);
                break;

            case quicr::TransportStatus::kShuttingDown:
                status_ = StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;
        }

        manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);
    }

    void PeerSession::OnNewConnection(const quicr::TransportConnId& conn_id, const quicr::TransportRemote& remote)
    {
        // Not used for outgoing connections. Incoming connections are handled by the server delegate
    }

    void PeerSession::OnRecvStream(const quicr::TransportConnId& conn_id,
                                   uint64_t stream_id,
                                   std::optional<quicr::DataContextId> data_ctx_id,
                                   const bool is_bidir)
    {
        auto stream_buf = transport_->GetStreamBuffer(conn_id, stream_id);

        if (stream_buf == nullptr) {
            return;
        }

        // Get common header
        if (stream_buf->Available(kCommonHeadersSize)) {
            auto bytes = stream_buf->Front(kCommonHeadersSize);

            auto version = bytes.front();
            auto type = ValueOf<uint16_t>({ bytes.begin() + 1, bytes.begin() + 3 });
            auto data_len = ValueOf<uint32_t>({ bytes.begin() + 3, bytes.begin() + 7 });

            if (stream_buf->Available(kCommonHeadersSize + data_len)) {
                stream_buf->Pop(kCommonHeadersSize);
                auto msg_bytes = stream_buf->Front(data_len);
                stream_buf->Pop(data_len);

                if (is_bidir) { // Control message
                    control_data_ctx_id_ = *data_ctx_id;

                    // Control Message
                    switch (static_cast<MsgType>(type)) {
                        case MsgType::kConnect: {
                            peering::Connect connect(msg_bytes);
                            SPDLOG_LOGGER_DEBUG(config_.logger_,
                                                "Connect from id: {} contact: {} mode: {}",
                                                NodeId().Value(connect.node_info.id),
                                                connect.node_info.contact,
                                                static_cast<int>(connect.mode));
                            remote_node_info_ = connect.node_info;

                            status_ = StatusValue::kConnected;
                            manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);

                            manager_.NodeReceived(GetSessionId(), connect.node_info, false);
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
                                                    "Connect error response from id: {} contact: {} error: {}",
                                                    NodeId().Value(connect_resp.node_info->id),
                                                    connect_resp.node_info->contact,
                                                    static_cast<int>(connect_resp.error));
                            }
                            status_ = StatusValue::kConnected;
                            manager_.SessionChanged(GetSessionId(), status_, remote_node_info_);
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

                } else {
                    // Data message
                }
            }

            SPDLOG_LOGGER_DEBUG(config_.logger_,
                                "Version: {} type: {} data_len: {} ctrl_data_ctx_id: {} recv_data_ctx_id: {}",
                                static_cast<int>(version),
                                type,
                                data_len,
                                control_data_ctx_id_,
                                *data_ctx_id);
        }
    }

    void PeerSession::OnRecvDgram(const quicr::TransportConnId& conn_id,
                                  std::optional<quicr::DataContextId> data_ctx_id)
    {
        for (int i = 0; i < 80; i++) {
            auto data = transport_->Dequeue(conn_id, data_ctx_id);

            if (!data) {
                return;
            }
        }
    }

    void PeerSession::OnConnectionMetricsSampled(const quicr::MetricsTimeStamp sample_time,
                                                 const quicr::TransportConnId conn_id,
                                                 const quicr::QuicConnectionMetrics& quic_connection_metrics)
    {
        metrics_.srtt_us = quic_connection_metrics.srtt_us.avg;
    }

}