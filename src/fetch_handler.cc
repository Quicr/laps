// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "fetch_handler.h"
#include "config.h"
#include <quicr/fetch_track_handler.h>
#include <quicr/server.h>

namespace laps {
    FetchTrackHandler::FetchTrackHandler(const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
                                         const quicr::FullTrackName& full_track_name,
                                         quicr::messages::ObjectPriority priority,
                                         quicr::messages::GroupOrder group_order,
                                         quicr::messages::GroupId start_group,
                                         quicr::messages::GroupId end_group,
                                         quicr::messages::GroupId start_object,
                                         quicr::messages::GroupId end_object)
      : quicr::FetchTrackHandler(full_track_name,
                                 priority,
                                 group_order,
                                 start_group,
                                 end_group,
                                 start_object,
                                 end_object)
      , publish_fetch_handler_(std::move(publish_fetch_handler))
    {
    }

    void FetchTrackHandler::StreamDataRecv(bool is_start,
                                           uint64_t stream_id,
                                           std::shared_ptr<const std::vector<uint8_t>> data)
    {
        if (is_start) {
            stream_buffer_.Clear();

            stream_buffer_.InitAny<quicr::messages::FetchHeader>();
            stream_buffer_.Push(*data);

            // Expect that on initial start of stream, there is enough data to process the stream headers

            auto& f_hdr = stream_buffer_.GetAny<quicr::messages::FetchHeader>();
            if (not(stream_buffer_ >> f_hdr)) {
                SPDLOG_ERROR("Not enough data to process new stream headers, stream is invalid len: {} / {}",
                             stream_buffer_.Size(),
                             data->size());
                // TODO: Add metrics to track this
                return;
            }
        } else {
            stream_buffer_.Push(*data);
        }

        if (not stream_buffer_.AnyHasValueB()) {
            stream_buffer_.InitAnyB<quicr::messages::FetchObject>();
        }

        auto& obj = stream_buffer_.GetAnyB<quicr::messages::FetchObject>();

        if (stream_buffer_ >> obj) {
            SPDLOG_TRACE("Received fetch_object subscribe_id: {} priority: {} "
                         "group_id: {} subgroup_id: {} object_id: {} data size: {}",
                         *GetSubscribeId(),
                         obj.publisher_priority,
                         obj.group_id,
                         obj.subgroup_id,
                         obj.object_id,
                         obj.payload.size());

            subscribe_track_metrics_.objects_received++;
            subscribe_track_metrics_.bytes_received += obj.payload.size();

            try {
                publish_fetch_handler_->PublishObject({ obj.group_id,
                                                        obj.object_id,
                                                        obj.subgroup_id,
                                                        obj.payload.size(),
                                                        obj.object_status,
                                                        obj.publisher_priority,
                                                        std::nullopt,
                                                        quicr::TrackMode::kStream,
                                                        obj.extensions,
                                                        obj.immutable_extensions },
                                                      obj.payload);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Caught exception trying to receive Fetch object. (error={})", e.what());
            }

            stream_buffer_.ResetAnyB<quicr::messages::FetchObject>();
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