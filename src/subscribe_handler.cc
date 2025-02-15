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

    void SubscribeTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data)
    {
        // Cache Object
        if (server_.cache_.count(GetTrackAlias().value()) == 0) {
            server_.cache_.insert(
              std::make_pair(GetTrackAlias().value(),
                             quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                               kDefaultCacheTimeQueueMaxDuration, 1, server_.config_.tick_service_ }));
        }

        auto& cache_entry = server_.cache_.at(GetTrackAlias().value());

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (auto group = cache_entry.Get(object_headers.group_id)) {
            group->insert(std::move(object));
        } else {
            cache_entry.Insert(object_headers.group_id, { std::move(object) }, kDefaultCacheTimeQueueObjectTtl);
        }
    }

    void SubscribeTrackHandler::StreamDataRecv(bool is_start,
                                               uint64_t stream_id,
                                               std::shared_ptr<const std::vector<uint8_t>> data)
    {
        is_datagram_ = false;

        if (stream_id > current_stream_id_) {
            current_stream_id_ = stream_id;
        } else if (stream_id < current_stream_id_) {
            SPDLOG_DEBUG(
              "Old stream data received, stream_id: {} is less than {}, ignoring", stream_id, current_stream_id_);
            return;
        }

        // Pipeline forward immediately to subscribers/peers
        ForwardReceivedData(is_start, data);

        // Process MoQ object from stream data
        if (is_start) {
            stream_buffer_.Clear();

            stream_buffer_.InitAny<quicr::messages::MoqStreamHeaderSubGroup>();
            stream_buffer_.Push(*data);
            stream_buffer_.Pop(); // Remove type header

            // Expect that on initial start of stream, there is enough data to process the stream headers

            auto& s_hdr = stream_buffer_.GetAny<quicr::messages::MoqStreamHeaderSubGroup>();
            if (not(stream_buffer_ >> s_hdr)) {
                SPDLOG_ERROR("Not enough data to process new stream headers, stream is invalid");
                // TODO: Add metrics to track this
                return;
            }
        } else {
            stream_buffer_.Push({ data->data(), data->size() });
        }

        auto& s_hdr = stream_buffer_.GetAny<quicr::messages::MoqStreamHeaderSubGroup>();

        if (not stream_buffer_.AnyHasValueB()) {
            stream_buffer_.InitAnyB<quicr::messages::MoqStreamSubGroupObject>();
        }

        auto& obj = stream_buffer_.GetAnyB<quicr::messages::MoqStreamSubGroupObject>();
        if (stream_buffer_ >> obj) {
            subscribe_track_metrics_.objects_received++;

            ObjectReceived({ s_hdr.group_id,
                             obj.object_id,
                             s_hdr.subgroup_id,
                             obj.payload.size(),
                             obj.object_status,
                             s_hdr.priority,
                             std::nullopt,
                             quicr::TrackMode::kStream,
                             obj.extensions },
                           obj.payload);

            stream_buffer_.ResetAnyB<quicr::messages::MoqStreamSubGroupObject>();
        }
    }

    void SubscribeTrackHandler::DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data)
    {
        is_datagram_ = true;

        // Pipeline forward immediately to subscribers/peers
        ForwardReceivedData(false, data);

        // Process MoQ object from stream data
        stream_buffer_.Clear();

        stream_buffer_.Push(*data);
        stream_buffer_.Pop(); // Remove type header

        quicr::messages::MoqObjectDatagram msg;
        if (stream_buffer_ >> msg) {
            subscribe_track_metrics_.objects_received++;
            subscribe_track_metrics_.bytes_received += msg.payload.size();
            ObjectReceived(
              {
                msg.group_id,
                msg.object_id,
                0, // datagrams don't have subgroups
                msg.payload.size(),
                quicr::ObjectStatus::kAvailable,
                msg.priority,
                std::nullopt,
                quicr::TrackMode::kDatagram,
                msg.extensions,
              },
              msg.payload);
        }
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

        peering::DataType d_type;
        auto track_mode = quicr::TrackMode::kStream;

        if (is_datagram_) {
            d_type = peering::DataType::kDatagram;
            track_mode = quicr::TrackMode::kDatagram;
        } else {
            d_type = peering::DataType::kExistingStream;

            if (is_new_stream) {
                d_type = peering::DataType::kNewStream;
            }
        }

        if (not is_from_peer_) {
            server_.peer_manager_.ClientDataRecv(*track_alias,
                                                 GetPriority(),
                                                 server_.config_.object_ttl_, /* TODO: Update this when MoQ adds TTL */
                                                 d_type,
                                                 data);
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
                auto pub_track_h = std::make_shared<PublishTrackHandler>(
                  sub_info.track_full_name,
                  track_mode,
                  sub_info.priority == 0 ? GetPriority() : sub_info.priority,
                  server_.config_.object_ttl_ /* TODO: Update this when MoQ adds TTL */);

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(connection_handle, sub_info.subscribe_id, pub_track_h, false);
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

    void SubscribeTrackHandler::SetFromPeer()
    {
        is_from_peer_ = true;
    }

}