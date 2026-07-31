// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "fetch_handler.h"
#include "config.h"
#include <quicr/handlers/fetch_track_handler.h>
#include <quicr/session.h>

namespace laps {
    FetchTrackHandler::FetchTrackHandler(const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
                                         const quicr::FullTrackName& full_track_name,
                                         std::uint64_t priority,
                                         std::optional<quicr::messages::GroupOrder> group_order,
                                         const quicr::messages::Location& start_location,
                                         const quicr::messages::FetchEndLocation& end_location)
      : quicr::FetchTrackHandler(full_track_name, priority, group_order, start_location, end_location)
      , publish_fetch_handler_(std::move(publish_fetch_handler))
    {
    }

    void FetchTrackHandler::StreamDataRecv(uint64_t stream_id, quicr::InitialStreamData&& initial_buffer)
    {
        subscribe_track_metrics_.bytes_received += initial_buffer.buffer.Size();

        auto [stream_it, inserted] = streams_.try_emplace(stream_id, std::move(initial_buffer.buffer));
        if (!inserted) {
            SPDLOG_ERROR("StreamDataRecv got new stream for existing Stream ID {}", stream_id);
            return;
        }

        stream_it->second.buffer.InitAny<quicr::messages::FetchHeader>();
        ProcessStreamStart(stream_id, stream_it->second);
    }

    void FetchTrackHandler::StreamDataRecv(uint64_t stream_id, std::shared_ptr<const std::vector<uint8_t>> data)
    {
        subscribe_track_metrics_.bytes_received += data->size();

        if (first_data_received_) {
            publish_fetch_handler_->ForwardPublishedData(false, 0, 0, std::move(data));
            return;
        }

        const auto stream_it = streams_.find(stream_id);
        if (stream_it == streams_.end()) {
            SPDLOG_ERROR("StreamDataRecv had no stream for expected Stream ID {}", stream_id);
            return;
        }

        stream_it->second.buffer.Push(*data);
        ProcessStreamStart(stream_id, stream_it->second);
    }

    void FetchTrackHandler::ProcessStreamStart(uint64_t stream_id, StreamContext& stream)
    {
        auto& f_hdr = stream.buffer.GetAny<quicr::messages::FetchHeader>();
        if (!(stream.buffer >> f_hdr)) {
            return;
        }

        SPDLOG_DEBUG("Fetch header added in rid: {} out rid: {} remaining data size: {}",
                     f_hdr.request_id,
                     *publish_fetch_handler_->GetRequestId(),
                     stream.buffer.Size());

        f_hdr.request_id = *publish_fetch_handler_->GetRequestId();
        auto bytes = std::make_shared<quicr::Bytes>();
        *bytes << f_hdr;

        const auto remaining_data = stream.buffer.Data();
        if (!remaining_data.empty()) {
            bytes->insert(bytes->end(), remaining_data.begin(), remaining_data.end());
        }

        publish_fetch_handler_->ForwardPublishedData(true, 0, 0, std::move(bytes));
        first_data_received_ = true;
        streams_.erase(stream_id);
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
