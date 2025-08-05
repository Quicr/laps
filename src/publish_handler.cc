// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "publish_handler.h"
#include <quicr/publish_track_handler.h>

namespace laps {
    /**
     * @brief Publish track handler
     * @details Publish track handler used for the publish command line option
     */
    PublishTrackHandler::PublishTrackHandler(const quicr::FullTrackName& full_track_name,
                                             quicr::TrackMode track_mode,
                                             uint8_t default_priority,
                                             uint32_t default_ttl,
                                             ClientManager& server)
      : quicr::PublishTrackHandler(full_track_name, track_mode, default_priority, default_ttl)
      , server_(server)
    {
    }

    void PublishTrackHandler::StatusChanged(Status status)
    {
        if (status == Status::kOk) {
            SPDLOG_INFO("Publish track alias {0} has subscribers", GetTrackAlias().value());
        } else {
            std::string reason = "";
            switch (status) {
                case Status::kNotConnected:
                    reason = "not connected";
                    break;
                case Status::kNotAnnounced:
                    reason = "not announced";
                    break;
                case Status::kAnnounceNotAuthorized:
                    reason = "not authorized";
                    break;
                case Status::kPendingAnnounceResponse:
                    reason = "pending announce response";
                    break;
                case Status::kNoSubscribers:
                    reason = "no subscribers";
                    break;
                case Status::kSendingUnannounce:
                    reason = "sending unannounce";
                    break;
                case Status::kPaused:
                    reason = "paused";
                    break;
                case Status::kSubscriptionUpdated:
                    reason = "subscription updated";
                    break;
                case Status::kNewGroupRequested:
                    reason = "new group requested";

                    // Update peering
                    // TODO: update peering with dampening
                    server_.peer_manager_.ClientSubscribeUpdate(GetTrackAlias().value(), true, true);

                    // Notify all publishers that there is a new group request
                    for (auto it = server_.state_.pub_subscribes.lower_bound({ GetTrackAlias().value(), 0 });
                         it != server_.state_.pub_subscribes.end();
                         ++it) {
                        auto& track_alias = it->first.first;
                        auto& pub_conn_id = it->first.second;

                        if (track_alias != GetTrackAlias().value()) {
                            break;
                        }

                        // dampen excessive floods
                        if (not server_.last_subscription_refresh_time.has_value()) {
                            server_.last_subscription_refresh_time = std::chrono::steady_clock::now();
                            break;
                        }

                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - server_.last_subscription_refresh_time.value())
                                         .count();
                        if (elapsed > server_.subscription_refresh_interval_ms) {
                            SPDLOG_INFO("Updating subscribe connection handler: {0} subscribe track_alias: {1}",
                                        pub_conn_id,
                                        track_alias);
                            server_.UpdateTrackSubscription(pub_conn_id, it->second, true);
                        }
                    }

                    break;
                default:
                    break;
            }
            SPDLOG_INFO("Publish track alias: {0} state change, reason: {1}", GetTrackAlias().value(), reason);
        }
    }

    void PublishTrackHandler::MetricsSampled(const quicr::PublishTrackMetrics& metrics)
    {
        SPDLOG_DEBUG("Metrics track_alias: {0}"
                     " objects sent: {1}"
                     " bytes sent: {2}"
                     " object duration us: {3}"
                     " queue discards: {4}"
                     " queue size: {5}",
                     GetTrackAlias().value(),
                     metrics.objects_published,
                     metrics.bytes_published,
                     metrics.quic.tx_object_duration_us.avg,
                     metrics.quic.tx_queue_discards,
                     metrics.quic.tx_queue_size.avg);
    }
}