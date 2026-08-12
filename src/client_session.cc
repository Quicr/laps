// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "client_session.h"
#include "client_manager.h"

namespace laps {

    void ClientSession::StatusChanged(Status)
    {
        if (close_reported_) {
            return;
        }

        const auto& connection = GetConnection();
        if (connection == nullptr) {
            return;
        }

        switch (connection->GetStatus()) {
            case quicr::Connection::Status::kReady:
            case quicr::Connection::Status::kConnecting:
            case quicr::Connection::Status::kShuttingDown:
                return;

            case quicr::Connection::Status::kRemoteRequestClose:
            case quicr::Connection::Status::kDisconnected:
            case quicr::Connection::Status::kIdleTimeout:
            case quicr::Connection::Status::kShutdown:
                break;
        }

        close_reported_ = true;
        manager_.ConnectionClosed(GetConnectionHandle());
    }

    void ClientSession::ClientSetupReceived(const quicr::ClientSetupAttributes& client_setup_attributes)
    {
        manager_.ClientSetupReceived(GetConnectionHandle(), client_setup_attributes);
    }

    void ClientSession::SubscribeTracksReceived(std::uint64_t data_ctx_id,
                                                const quicr::TrackNamespace& prefix_namespace,
                                                const quicr::SubscribeNamespaceAttributes& attributes)
    {
        manager_.SubscribeTracksReceived(GetConnectionHandle(), data_ctx_id, prefix_namespace, attributes);
    }

    void ClientSession::UnsubscribeNamespaceReceived(const quicr::TrackNamespace& prefix_namespace)
    {
        manager_.UnsubscribeNamespaceReceived(GetConnectionHandle(), prefix_namespace);
    }

    void ClientSession::PublishNamespaceReceived(const quicr::TrackNamespace& track_namespace,
                                                 const quicr::PublishNamespaceAttributes& attributes)
    {
        manager_.PublishNamespaceReceived(GetConnectionHandle(), track_namespace, attributes);
    }

    std::vector<std::uint64_t> ClientSession::PublishNamespaceDoneReceived(std::uint64_t request_id)
    {
        return manager_.PublishNamespaceDoneReceived(GetConnectionHandle(), request_id);
    }

    void ClientSession::PublishReceived(std::uint64_t request_id,
                                        const quicr::PublishAttributes& publish_attributes,
                                        std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler)
    {
        manager_.PublishReceived(GetConnectionHandle(), request_id, publish_attributes, std::move(sub_ns_handler));
    }

    void ClientSession::PublishDoneReceived(std::uint64_t request_id)
    {
        manager_.PublishDoneReceived(GetConnectionHandle(), request_id);
    }

    void ClientSession::SubscribeReceived(std::uint64_t request_id,
                                          const quicr::FullTrackName& track_full_name,
                                          const quicr::SubscribeAttributes& attributes)
    {
        manager_.SubscribeReceived(GetConnectionHandle(), request_id, track_full_name, attributes);
    }

    void ClientSession::UnsubscribeReceived(std::uint64_t request_id)
    {
        manager_.UnsubscribeReceived(GetConnectionHandle(), request_id);
    }

    void ClientSession::NewGroupRequested(const quicr::FullTrackName& track_full_name, std::uint64_t group_id)
    {
        manager_.NewGroupRequested(track_full_name, group_id);
    }

    void ClientSession::TrackStatusReceived(std::uint64_t request_id, const quicr::FullTrackName& track_full_name)
    {
        manager_.TrackStatusReceived(GetConnectionHandle(), request_id, track_full_name);
    }

    void ClientSession::StandaloneFetchReceived(std::uint64_t request_id,
                                                const quicr::FullTrackName& track_full_name,
                                                const quicr::StandaloneFetchAttributes& attributes)
    {
        manager_.StandaloneFetchReceived(GetConnectionHandle(), request_id, track_full_name, attributes);
    }

    void ClientSession::JoiningFetchReceived(std::uint64_t request_id,
                                             const quicr::FullTrackName& track_full_name,
                                             const quicr::JoiningFetchAttributes& attributes)
    {
        manager_.JoiningFetchReceived(GetConnectionHandle(), request_id, track_full_name, attributes);
    }

    void ClientSession::FetchCancelReceived(std::uint64_t request_id)
    {
        manager_.FetchCancelReceived(GetConnectionHandle(), request_id);
    }

    void ClientSession::MetricsSampled(const quicr::ConnectionMetrics& metrics)
    {
        manager_.MetricsSampled(GetConnectionHandle(), metrics);
    }

} // namespace laps
