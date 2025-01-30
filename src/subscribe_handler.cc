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
      : quicr::SubscribeTrackHandler(full_track_name, priority, group_order, quicr::messages::FilterType::LatestObject)
      , server_(server)
    {
    }

    void SubscribeTrackHandler::ObjectReceived([[maybe_unused]] const quicr::ObjectHeaders& object_headers,
                                               [[maybe_unused]] quicr::BytesSpan data)
    {
    }

    void SubscribeTrackHandler::StreamDataRecv(bool is_start, std::shared_ptr<const std::vector<uint8_t>> data)
    {
        is_datagram_ = false;

        ForwardReceivedData(is_start, data);
    }

    void SubscribeTrackHandler::DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data)
    {
        is_datagram_ = true;

        ForwardReceivedData(false, data);
    }

    void SubscribeTrackHandler::ForwardReceivedData(bool is_new_stream,
                                                    std::shared_ptr<const std::vector<uint8_t>> data)
    {
        std::lock_guard<std::mutex> _(server_.state_.state_mutex);

        auto track_alias = GetTrackAlias();
        if (!track_alias.has_value()) {
            SPDLOG_DEBUG("Data without valid track alias");
            return;
        }

        // Send to peers
        peering::DataObjectType d_type;
        auto track_mode = quicr::TrackMode::kStream;

        if (is_datagram_) {
            d_type = peering::DataObjectType::kDatagram;
            track_mode = quicr::TrackMode::kDatagram;
        } else {
            d_type = peering::DataObjectType::kExistingStream;

            if (is_new_stream) {
                d_type = peering::DataObjectType::kNewStream;
            }
        }

        server_.peer_manager_.ClientDataObject(*track_alias,
                                               GetPriority(),
                                               server_.config_.object_ttl_, /* TODO: Update this when MoQ adds TTL */
                                               d_type,
                                               data);

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
                  track_mode,
                  sub_info.priority == 0 ? GetPriority() : sub_info.priority,
                  server_.config_.object_ttl_ /* TODO: Update this when MoQ adds TTL */);

                auto&& cache_message_callback = [&server = server_, tnsh = quicr::TrackHash(sub_info.track_full_name)](
                                                  uint8_t priority,
                                                  uint32_t ttl,
                                                  [[maybe_unused]] bool stream_header_needed,
                                                  uint64_t group_id,
                                                  uint64_t subgroup_id,
                                                  uint64_t object_id,
                                                  std::optional<quicr::Extensions> extensions,
                                                  quicr::BytesSpan data) {
                    /* TODO: Caching moves to Object Receive handling of this subscribe handler
                    if (server.cache_.count(tnsh) == 0) {
                        server.cache_.insert(
                          std::make_pair(tnsh,
                                         quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                                           server.cache_duration_ms_, 1, server.GetTickService() }));
                    }

                    auto& cache_entry = server.cache_.at(tnsh);

                    CacheObject object{
                        quicr::ObjectHeaders{
                          group_id,
                          object_id,
                          subgroup_id,
                          data.size(),
                          quicr::ObjectStatus::kAvailable,
                          priority,
                          ttl,
                          std::nullopt,
                          extensions,
                        },
                        { data.begin(), data.end() },
                    };

                    if (auto group = cache_entry.Get(group_id)) {
                        group->insert(std::move(object));
                    } else {
                        cache_entry.Insert(group_id, { std::move(object) }, server.cache_duration_ms_);
                    }
                    */
                };

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(
                  connection_handle, sub_info.subscribe_id, pub_track_h, false, std::move(cache_message_callback));
                sub_info.publish_handler = pub_track_h;
            }

            sub_info.publish_handler->ForwardPublishedData(is_new_stream, data);
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