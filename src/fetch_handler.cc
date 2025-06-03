// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "fetch_handler.h"
#include "config.h"
#include <quicr/fetch_track_handler.h>
#include <quicr/server.h>

namespace laps {
    FetchTrackHandler::FetchTrackHandler(const quicr::FullTrackName& full_track_name,
                                         quicr::messages::ObjectPriority priority,
                                         quicr::messages::GroupOrder group_order,
                                         quicr::messages::GroupId start_group,
                                         quicr::messages::GroupId end_group,
                                         quicr::messages::GroupId start_object,
                                         quicr::messages::GroupId end_object,
                                         ClientManager& server)
      : quicr::FetchTrackHandler(full_track_name,
                                 priority,
                                 group_order,
                                 start_group,
                                 end_group,
                                 start_object,
                                 end_object)
      , server_(server)
    {
    }

    void FetchTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data)
    {
        std::lock_guard<std::mutex> _(server_.state_.state_mutex);
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
                default:
                    break;
            }
            SPDLOG_DEBUG("Track alias: {0} fetch status change reason: {1}", GetTrackAlias().value(), reason);
        }
    }
}