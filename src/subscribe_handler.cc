// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_handler.h"
#include "config.h"
#include "publish_handler.h"
#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

namespace laps {
    SubscribeTrackHandler::SubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                                 quicr::messages::ObjectPriority priority,
                                                 quicr::messages::GroupOrder group_order,
                                                 LapsServer& server)
      : quicr::SubscribeTrackHandler(full_track_name, priority, group_order)
      , server_(server)
    {
    }

    void SubscribeTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data)
    {
        std::lock_guard<std::mutex> _(server_.state_.state_mutex);

        auto track_alias = GetTrackAlias();
        if (!track_alias.has_value()) {
            SPDLOG_DEBUG("Data without valid track alias");
            return;
        }

        // Fanout object to subscribers
        for (auto it = server_.state_.subscribes.lower_bound({ track_alias.value(), 0 });
             it != server_.state_.subscribes.end();
             ++it) {
            auto& [key, sub_info] = *it;
            const auto& sub_track_alias = key.first;
            const auto& connection_handle = key.second;

            if (sub_track_alias != track_alias.value())
                break;

            if (sub_info.publish_handler == nullptr) {
                // Create the publish track handler and bind it on first object received
                auto pub_track_h =
                  std::make_shared<PublishTrackHandler>(sub_info.track_full_name,
                                                        *object_headers.track_mode,
                                                        sub_info.priority == 0 ? *object_headers.priority : sub_info.priority,
                                                        object_headers.ttl.has_value() ? *object_headers.ttl : 5000);

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(connection_handle, sub_info.subscribe_id, pub_track_h);
                sub_info.publish_handler = pub_track_h;
            }

            sub_info.publish_handler->PublishObject(object_headers, data);
        }
    }

    void SubscribeTrackHandler::StatusChanged(Status status)
    {
        if (status == Status::kOk) {
            SPDLOG_INFO("Track alias: {0} is subscribed", GetTrackAlias().value());
        } else {
            std::string reason = "";
            switch (status) {
                case Status::kNotConnected:
                    reason = "not connected";
                    break;
                case Status::kSubscribeError:
                    reason = "subscribe error";
                    break;
                case Status::kNotAuthorized:
                    reason = "not authorized";
                    break;
                case Status::kNotSubscribed:
                    reason = "not subscribed";
                    break;
                case Status::kPendingSubscribeResponse:
                    reason = "pending subscribe response";
                    break;
                case Status::kSendingUnsubscribe:
                    reason = "unsubscribing";
                    break;
                default:
                    break;
            }
            SPDLOG_DEBUG("Track alias: {0} subscribe status change reason: {1}", GetTrackAlias().value(), reason);
        }
    }
}