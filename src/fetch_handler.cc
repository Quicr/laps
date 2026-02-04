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
                                         const quicr::messages::Location& start_location,
                                         const quicr::messages::FetchEndLocation& end_location)
      : quicr::FetchTrackHandler(full_track_name, priority, group_order, start_location, end_location)
      , publish_fetch_handler_(std::move(publish_fetch_handler))
    {
    }

    void FetchTrackHandler::StreamDataRecv(bool is_start,
                                           uint64_t stream_id,
                                           std::shared_ptr<const std::vector<uint8_t>> data)
    {
        subscribe_track_metrics_.bytes_received += data->size();
        auto& stream = streams_[stream_id];

        if (is_start) {
            stream.buffer.Clear();

            stream.buffer.InitAny<quicr::messages::FetchHeader>();
            stream.buffer.Push(*data);

            // Expect that on initial start of stream, there is enough data to process the stream headers

            auto f_hdr = stream.buffer.GetAny<quicr::messages::FetchHeader>();
            if (not(stream.buffer >> f_hdr)) {
                SPDLOG_ERROR("Not enough data to process new stream headers, stream is invalid len: {} / {}",
                             stream.buffer.Size(),
                             data->size());
                // TODO: Add metrics to track this
                return;
            }

            size_t header_size = data->size() - stream.buffer.Size();

            SPDLOG_DEBUG("Fetch header added in rid: {} out rid: {} data sz: {} sbuf_size: {} header size: {}",
                         f_hdr.request_id,
                         *publish_fetch_handler_->GetRequestId(),
                         data->size(),
                         stream.buffer.Size(),
                         header_size);

            f_hdr.request_id = *publish_fetch_handler_->GetRequestId();
            auto bytes = std::make_shared<quicr::Bytes>();
            *bytes << f_hdr;

            if (header_size < data->size()) {
                bytes->insert(bytes->end(), data->begin() + header_size, data->end());
                stream.buffer.Pop(stream.buffer.Size());
            }

            publish_fetch_handler_->ForwardPublishedData(true, 0, 0, std::move(bytes));
        } else {
            publish_fetch_handler_->ForwardPublishedData(false, 0, 0, std::move(data));
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