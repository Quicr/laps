// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_handler.h"
#include "config.h"
#include "publish_handler.h"
#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

namespace laps {
    LapsSubscribeTrackHandler::LapsSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                                         LapsServer& server)
      : SubscribeTrackHandler(full_track_name)
      , server_(server)
    {
    }

    void LapsSubscribeTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data)
    {
        std::lock_guard<std::mutex> _(server_.state_.state_mutex);

        auto track_alias = GetTrackAlias();
        if (!track_alias.has_value()) {
            SPDLOG_DEBUG("Data without valid track alias");
            return;
        }

        auto sub_it = server_.state_.subscribes.find(track_alias.value());

        if (sub_it == server_.state_.subscribes.end()) {
            SPDLOG_INFO("No subscribes, not relaying data size: {0} ", data.size());
            return;
        }

        for (auto& [conn_id, sphi] : sub_it->second) {
            if (sphi.publish_handler == nullptr) {
                // Create the publish track handler and bind it on first object received
                auto pub_track_h = std::make_shared<LapsPublishTrackHandler>(
                  sphi.track_full_name,
                  *object_headers.track_mode,
                  *object_headers.priority,
                  object_headers.ttl.has_value() ? *object_headers.ttl : 5000);
                sphi.publish_handler = pub_track_h;

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(conn_id, sphi.subscribe_id, pub_track_h);
            }
            sphi.publish_handler->PublishObject(object_headers, data);
        }
    }

    void LapsSubscribeTrackHandler::StatusChanged(Status status)
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
            SPDLOG_INFO("Track alias: {0} failed to subscribe reason: {1}", GetTrackAlias().value(), reason);
        }
    }
}