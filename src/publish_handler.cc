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
                                                     uint32_t default_ttl)
      : quicr::PublishTrackHandler(full_track_name, track_mode, default_priority, default_ttl)
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
                default:
                    break;
            }
            SPDLOG_INFO("Publish track alias: {0} not ready, reason: {1}", GetTrackAlias().value(), reason);
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