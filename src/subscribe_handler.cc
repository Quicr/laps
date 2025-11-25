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
                                                 ClientManager& server,
                                                 bool is_publisher_initiated)
      : quicr::SubscribeTrackHandler(full_track_name,
                                     priority,
                                     group_order,
                                     quicr::messages::FilterType::kLargestObject,
                                     std::nullopt,
                                     is_publisher_initiated)
      , server_(server)
    {
    }

    void SubscribeTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data)
    {
        auto self_connection_handle = GetConnectionId();

        // Cache Object
        if (server_.cache_.count(GetTrackAlias().value()) == 0) {
            server_.cache_.insert(std::make_pair(GetTrackAlias().value(),
                                                 quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                                                   server_.cache_duration_ms_, 1000, server_.config_.tick_service_ }));
        }

        auto& cache_entry = server_.cache_.at(GetTrackAlias().value());

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (pending_new_group_request_id_.has_value() && current_group_id_ != object_headers.group_id) {
            pending_new_group_request_id_.reset();
        }

        current_group_id_ = object_headers.group_id;
        current_subgroup_id_ = object_headers.subgroup_id;

        if (auto group = cache_entry.Get(object_headers.group_id)) {
            group->insert(std::move(object));
        } else {
            cache_entry.Insert(object_headers.group_id, { std::move(object) }, server_.cache_duration_ms_);
        }

        try {
            // Fanout object to subscribers
            for (auto it = server_.state_.subscribes.lower_bound({ GetTrackAlias().value(), 0 });
                 it != server_.state_.subscribes.end();
                 ++it) {
                auto& [key, sub_info] = *it;
                const auto& sub_track_alias = key.first;

                if (sub_track_alias != GetTrackAlias().value())
                    break;

                if (sub_info.publish_handlers[self_connection_handle] == nullptr) {
                    continue;
                }

                if (sub_info.publish_handlers[self_connection_handle]->pipeline_) {
                    continue;
                }

                sub_info.publish_handlers[self_connection_handle]->PublishObject(object_headers, data);
                sub_info.publish_handlers[self_connection_handle]->pipeline_ = true;
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Caught exception trying to publish. (error={})", e.what());
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
        if (is_start || not stream_buffer_.AnyHasValue()) {
            stream_buffer_.Clear();

            stream_buffer_.InitAny<quicr::messages::StreamHeaderSubGroup>();
            stream_buffer_.Push(*data);

            // Expect that on initial start of stream, there is enough data to process the stream headers

            auto& s_hdr = stream_buffer_.GetAny<quicr::messages::StreamHeaderSubGroup>();
            if (not(stream_buffer_ >> s_hdr)) {
                SPDLOG_ERROR("Not enough data to process new stream headers, stream is invalid");
                // TODO: Add metrics to track this
                return;
            }
        } else {
            stream_buffer_.Push({ data->data(), data->size() });
        }

        auto& s_hdr = stream_buffer_.GetAny<quicr::messages::StreamHeaderSubGroup>();

        while (true) {
            if (not stream_buffer_.AnyHasValueB()) {
                stream_buffer_.InitAnyB<quicr::messages::StreamSubGroupObject>();
            }

            auto& obj = stream_buffer_.GetAnyB<quicr::messages::StreamSubGroupObject>();
            obj.stream_type = s_hdr.type;
            const auto subgroup_properties = quicr::messages::StreamHeaderProperties(s_hdr.type);
            if (stream_buffer_ >> obj) {
                subscribe_track_metrics_.objects_received++;

                if (next_object_id_.has_value()) {
                    if (current_group_id_ != s_hdr.group_id || current_subgroup_id_ != s_hdr.subgroup_id) {
                        next_object_id_ = obj.object_delta;
                    } else {
                        *next_object_id_ += obj.object_delta;
                    }
                } else {
                    next_object_id_ = obj.object_delta;
                }

                if (pending_new_group_request_id_.has_value() && current_group_id_ != s_hdr.group_id) {
                    pending_new_group_request_id_.reset();
                }

                current_group_id_ = s_hdr.group_id;
                current_subgroup_id_ = s_hdr.subgroup_id.value();

                if (!s_hdr.subgroup_id.has_value()) {
                    if (subgroup_properties.subgroup_id_type != quicr::messages::SubgroupIdType::kSetFromFirstObject) {
                        SPDLOG_ERROR("Bad stream header type when no subgroup ID: {0}",
                                     static_cast<std::uint8_t>(s_hdr.type));
                        return;
                    }
                    s_hdr.subgroup_id = next_object_id_;
                }

                ObjectReceived({ s_hdr.group_id,
                                 next_object_id_.value(),
                                 s_hdr.subgroup_id.value(),
                                 obj.payload.size(),
                                 obj.object_status,
                                 s_hdr.priority,
                                 std::nullopt,
                                 quicr::TrackMode::kStream,
                                 obj.extensions,
                                 obj.immutable_extensions },
                               obj.payload);

                *next_object_id_ += 1;
                stream_buffer_.ResetAnyB<quicr::messages::StreamSubGroupObject>();
            }

            break; // Not complete, wait for more data
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

        quicr::messages::ObjectDatagram msg;
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
        auto self_connection_handle = GetConnectionId();

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

                if (GetDeliveryTimeout().count() == 0) {
                    // Use default if delivery timeout is not set
                    SetDeliveryTimeout(std::chrono::milliseconds(server_.config_.object_ttl_));
                }
            }
        }

        if (not is_from_peer_) {
            server_.peer_manager_.ClientDataRecv(
              *track_alias, GetPriority(), GetDeliveryTimeout().count(), d_type, data);
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

            if (!sub_info.publish_handlers.contains(self_connection_handle)) {
                // Create the publish track handler and bind it on first object received
                auto pub_track_h = std::make_shared<PublishTrackHandler>(
                  sub_info.track_full_name,
                  track_mode,
                  sub_info.priority == 0 ? GetPriority() : sub_info.priority,
                  sub_info.object_ttl == 0 ? server_.config_.object_ttl_ : sub_info.object_ttl,
                  server_);

                // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
                server_.BindPublisherTrack(
                  connection_handle, self_connection_handle, sub_info.request_id, pub_track_h, false);
                sub_info.publish_handlers[self_connection_handle] = pub_track_h;
                continue;
            }

            const auto pub_track_h = sub_info.publish_handlers[self_connection_handle];
            if (is_new_stream && pub_track_h->SentFirstObject()) {
                pub_track_h->pipeline_ = true;
            } else if (not pub_track_h->pipeline_) {
                continue;
            }

            sub_info.publish_handlers[self_connection_handle]->ForwardPublishedData(is_new_stream, data);
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
                    if (GetTrackAlias().has_value()) {
                        auto& anno_tracks =
                          server_.state_.namespace_active[{ GetFullTrackName().name_space, GetConnectionId() }];
                        anno_tracks.erase(GetTrackAlias().value());
                        server_.state_.pub_subscribes.erase({ GetTrackAlias().value(), GetConnectionId() });
                    }
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
                case Status::kPaused:
                    reason = "paused";
                    break;
                case Status::kNewGroupRequested:
                    reason = "new group requested";
                    break;
                case Status::kDoneByFin:
                    reason = "Done by FIN";
                    status = Status::kOk;
                    break;
                case Status::kDoneByReset:
                    reason = "Done by Reset";
                    status = Status::kOk;
                    break;
                default:
                    break;
            }
            SPDLOG_DEBUG("Track alias: {} subscribe status change reason: {} status: {}",
                         GetTrackAlias().value(),
                         reason,
                         static_cast<int>(status));
        }
    }

    void SubscribeTrackHandler::SetFromPeer()
    {
        is_from_peer_ = true;
    }

}