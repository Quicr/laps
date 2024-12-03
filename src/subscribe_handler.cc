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
                                                 ClientManager& server)
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

        // Send to peers
        peering::DataObjectType d_type;
        if (object_headers.track_mode.has_value() && *object_headers.track_mode == quicr::TrackMode::kDatagram) {
            d_type = peering::DataObjectType::kDatagram;
        } else {
            d_type = peering::DataObjectType::kExistingStream;

            if (prev_group_id_ != object_headers.group_id || prev_subgroup_id_ != object_headers.subgroup_id) {
                d_type = peering::DataObjectType::kNewStream;
            }
        }

        prev_group_id_ = object_headers.group_id;
        prev_subgroup_id_ = object_headers.subgroup_id;

        std::vector<uint8_t> peer_data;
        quicr::messages::MoqStreamSubGroupObject object;
        object.object_id = object_headers.object_id;
        object.extensions = object_headers.extensions;
        object.payload.assign(data.begin(), data.end());
        peer_data << object;

        server_.peer_manager_.ClientDataObject(*track_alias,
                                               object_headers.priority.has_value() ? *object_headers.priority : 2,
                                               object_headers.ttl.has_value() ? *object_headers.ttl : 2000,
                                               object_headers.group_id,
                                               object_headers.subgroup_id,
                                               d_type,
                                               peer_data);

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
                auto pub_track_h = std::make_shared<PublishTrackHandler>(
                  sub_info.track_full_name,
                  *object_headers.track_mode,
                  sub_info.priority == 0 ? *object_headers.priority : sub_info.priority,
                  object_headers.ttl.has_value() ? *object_headers.ttl : 5000);

                auto&& cache_message_callback =
                  [&, tnsh = quicr::TrackHash(sub_info.track_full_name)](uint8_t priority,
                                                                         uint32_t ttl,
                                                                         bool stream_header_needed,
                                                                         uint64_t group_id,
                                                                         uint64_t subgroup_id,
                                                                         uint64_t object_id,
                                                                         std::optional<quicr::Extensions> extensions,
                                                                         quicr::BytesSpan data) {
                      if (server_.cache_.count(tnsh) == 0) {
                          server_.cache_.insert(
                            std::make_pair(tnsh,
                                           quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                                             ttl, 1, server_.GetTickService() }));
                      }

                      auto& cache_entry = server_.cache_.at(tnsh);

                      CacheObject object{
                          priority,    ttl,       stream_header_needed, group_id,
                          subgroup_id, object_id, extensions,           { data.begin(), data.end() },
                      };

                      if (auto group = cache_entry.Get(group_id)) {
                          group->insert(std::move(object));
                      } else {
                          cache_entry.Insert(group_id, { std::move(object) }, ttl);
                      }
                  };

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(
                  connection_handle, sub_info.subscribe_id, pub_track_h, std::move(cache_message_callback));
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
                case Status::kError:
                    reason = "subscribe error";
                    break;
                case Status::kNotAuthorized:
                    reason = "not authorized";
                    break;
                case Status::kNotSubscribed:
                    reason = "not subscribed";
                    break;
                case Status::kPendingResponse:
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