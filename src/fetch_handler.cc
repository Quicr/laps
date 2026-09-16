// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "fetch_handler.h"
#include "config.h"
#include <quicr/handlers/fetch_track_handler.h>

namespace laps {
    FetchTrackHandler::FetchTrackHandler(const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
                                         const quicr::FullTrackName& full_track_name,
                                         std::uint8_t priority,
                                         const quicr::messages::Location& start_location,
                                         const quicr::messages::FetchEndLocation& end_location,
                                         quicr::messages::GroupOrder group_order)
      : quicr::FetchTrackHandler(full_track_name, priority, start_location, end_location, group_order)
      , publish_fetch_handler_(std::move(publish_fetch_handler))
    {
    }

    void FetchTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers,
                                           quicr::BytesSpan data,
                                           std::optional<quicr::messages::StreamHeaderProperties> stream_mode)
    {
        if (!initial_stream_data_forwarded_) {
            publish_fetch_handler_->PublishObject(object_headers, data, stream_mode);
            initial_stream_data_forwarded_ = true;
            return;
        }

        auto bytes = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
        if (publish_fetch_handler_->ForwardPublishedData(false, 0, 0, std::move(bytes)) !=
            quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
            publish_fetch_handler_->PublishObject(object_headers, data, stream_mode);
        }
    }

    void FetchTrackHandler::StatusChanged(Status status)
    {
        if (status == Status::kOk) {
            SPDLOG_INFO("Track alias: {0} is fetched", GetTrackAlias().value());
        } else {
            std::string reason = "";
            switch (status) {
                case Status::kNotConnected:
                    reason = "not connected";
                    break;
                case Status::kError:
                    reason = "fetch error";
                    break;
                case Status::kNotAuthorized:
                    reason = "not authorized";
                    break;
                case Status::kNotSubscribed:
                    reason = "not subscribed";
                    break;
                case Status::kPendingResponse:
                    reason = "pending fetch response";
                    break;
                case Status::kDoneByFin:
                    reason = "fetch done by FIN";
                    break;
                case Status::kDoneByReset:
                    reason = "fetch done by RESET";
                    break;
                default:
                    break;
            }
            SPDLOG_DEBUG("Track alias: {0} fetch status change reason: {1}", GetTrackAlias().value(), reason);
        }
    }
}
