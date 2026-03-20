// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_handler.h"
#include "config.h"
#include "publish_handler.h"
#include "publish_namespace_handler.h"

#include <quicr/server.h>
#include <quicr/subscribe_track_handler.h>

namespace laps {
    SubscribeTrackHandler::SubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                                 quicr::messages::ObjectPriority priority,
                                                 quicr::messages::GroupOrder group_order,
                                                 ClientManager& server,
                                                 std::weak_ptr<quicr::TickService> tick_service,
                                                 bool is_publisher_initiated)
      : quicr::SubscribeTrackHandler(full_track_name,
                                     priority,
                                     group_order,
                                     std::monostate{},
                                     std::nullopt,
                                     is_publisher_initiated)
      , server_(server)
      , tick_service_(std::move(tick_service))
    {
        tracked_properties_value_.emplace(12, 0);
    }

    SubscribeTrackHandler::~SubscribeTrackHandler()
    {
        for (auto& [conn_handle, handler] : subscribers) {
            server_.UnbindPublisherTrack(conn_handle, GetConnectionId(), handler);
        }
    }

    void SubscribeTrackHandler::AddSubscribeNamespace(std::shared_ptr<PublishNamespaceHandler> handler)
    {
        auto th = quicr::TrackHash(handler->GetFullTrackName());
        const auto it = sub_namespaces_[th.track_fullname_hash].find(handler->GetConnectionId());
        if (it != sub_namespaces_[th.track_fullname_hash].end()) {
            // Duplicate
            return;
        }

        sub_namespaces_[th.track_fullname_hash].emplace(handler->GetConnectionId(), handler);

        Resume();
    }

    void SubscribeTrackHandler::RemoveSubscribeNamespace(std::shared_ptr<PublishNamespaceHandler> handler)
    {
        auto th = quicr::TrackHash(handler->GetFullTrackName());

        auto it = sub_namespaces_.find(th.track_fullname_hash);
        if (it != sub_namespaces_.end()) {
            it->second.erase(handler->GetConnectionId());

            if (it->second.empty()) {
                sub_namespaces_.erase(it);
            }
        }
    }

    void SubscribeTrackHandler::AddSubscriber(quicr::ConnectionHandle conn_handle,
                                              quicr::messages::RequestID request_id,
                                              uint8_t priority,
                                              std::chrono::milliseconds delivery_timeout,
                                              quicr::messages::Location start_location)
    {

        if (subscribers.contains(conn_handle)) {
            // Duplicate
            return;
        }

        auto pub_track_h = std::make_shared<PublishTrackHandler>(
          GetFullTrackName(),
          is_datagram_ ? quicr::TrackMode::kDatagram : quicr::TrackMode::kStream,
          priority == 0 ? GetPriority() : priority,
          delivery_timeout.count() == 0
            ? GetDeliveryTimeout().value_or(std::chrono::milliseconds(server_.config_.object_ttl_)).count()
            : delivery_timeout.count(),
          start_location,
          server_);

        // Create a subscribe track that will be used by the relay to send to subscriber for matching objects
        server_.BindPublisherTrack(conn_handle, GetConnectionId(), request_id, pub_track_h, false);

        subscribers.emplace(conn_handle, pub_track_h);

        Resume();
    }

    void SubscribeTrackHandler::RemoveSubscriber(quicr::ConnectionHandle conn_handle)
    {
        auto it = subscribers.find(conn_handle);
        if (it == subscribers.end()) {
            return;
        }

        server_.UnbindPublisherTrack(conn_handle, GetConnectionId(), it->second);
        subscribers.erase(conn_handle);
    }

    void SubscribeTrackHandler::UpdateTrackedProperties(std::optional<quicr::Extensions> extensions,
                                                        std::optional<quicr::Extensions> immutable_extensions)
    {
        auto update = [ta = GetTrackAlias().value(),
                       conn_id = GetConnectionId(),
                       ticks = tick_service_.lock(),
                       ranking = track_ranking_.lock()](
                        uint64_t prop, PublishNamespaceHandler::TrackPropertyValue& value, uint64_t recv_value) {
            quicr::TickService::TickType cur_tick{ 0 };
            if (ticks != nullptr) {
                cur_tick = ticks->Milliseconds();
            }

            if (value.latest_value != recv_value || cur_tick - value.latest_tick_ms > kRefreshRankingIntervalMs) {
                value.latest_value = recv_value;
                value.latest_tick_ms = cur_tick;

                if (ranking) {
                    ranking->UpdateValue(ta, prop, value.latest_value, value.latest_tick_ms, conn_id);
                }
            }
        };

        if (extensions) {
            for (auto& [prop, value] : tracked_properties_value_) {
                if (prop % 2 != 0) {
                    continue;
                }

                if (extensions->contains(prop)) {
                    update(prop, value, uint64_t(quicr::UintVar(extensions->at(prop).front())));
                    continue;
                }
                if (immutable_extensions.has_value() && immutable_extensions->contains(prop)) {
                    update(prop, value, uint64_t(quicr::UintVar(immutable_extensions->at(prop).front())));
                }
            }
        }
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

        // Update tracked properties
        UpdateTrackedProperties(object_headers.extensions, object_headers.immutable_extensions);

        auto& cache_entry = server_.cache_.at(GetTrackAlias().value());

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (pending_new_group_request_id_.has_value() &&
            (object_headers.group_id == 0 || object_headers.group_id > *pending_new_group_request_id_)) {
            pending_new_group_request_id_.reset();
        }

        if (auto group = cache_entry.Get(object_headers.group_id)) {
            group->insert(std::move(object));
        } else {
            cache_entry.Insert(object_headers.group_id, { std::move(object) }, server_.cache_duration_ms_);
        }

        try {
            // Fanout object to subscribe namespaces
            for (const auto [_, conn_subs] : sub_namespaces_) {
                for (const auto& [_, handler] : conn_subs) {
                    handler->PublishObject(GetTrackAlias().value(), object_headers, data);
                }
            }

            // Fanout object to subscribers
            for (auto& [conn_handle, pub_handler] : subscribers) {

                if (pub_handler->SentFirstObject(object_headers.group_id, object_headers.subgroup_id)) {
                    // pipeline this connection/subscriber via forward data method instead
                    continue;
                }

                pub_handler->SetDefaultTrackMode(is_datagram_ ? quicr::TrackMode::kDatagram
                                                              : quicr::TrackMode::kStream);

                if (object.headers.group_id >= pub_handler->start_location_.group &&
                    object.headers.object_id >= pub_handler->start_location_.object) {
                    pub_handler->PublishObject(object_headers, data);
                }
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

        auto& stream = streams_[stream_id];

        // Process MoQ object from stream data
        if (is_start) {
            stream.buffer.Clear();

            stream.buffer.InitAny<quicr::messages::StreamHeaderSubGroup>();
            stream.buffer.Push(*data);

            // Expect that on initial start of stream, there is enough data to process the stream headers
            auto& s_hdr = stream.buffer.GetAny<quicr::messages::StreamHeaderSubGroup>();
            if (not(stream.buffer >> s_hdr)) {
                SPDLOG_ERROR("Not enough data to process new stream headers, stream is invalid");
                // TODO: Add metrics to track this
                return;
            }

            // Adapt publisher track alias to normalized track alias uses by subscribers
            if (GetReceivedTrackAlias().value() != GetTrackAlias().value()) {
                s_hdr.track_alias = GetTrackAlias().value();
                quicr::Bytes updated_s_hdr;

                auto updated_data = std::make_shared<std::vector<uint8_t>>();
                *updated_data << s_hdr;

                updated_data->insert(
                  updated_data->end(), data->begin() + (data->size() - stream.buffer.Size()), data->end());

                ForwardReceivedData(is_start, s_hdr.group_id, s_hdr.subgroup_id.value_or(0), updated_data);
            } else {
                ForwardReceivedData(is_start, s_hdr.group_id, s_hdr.subgroup_id.value_or(0), data);
            }

        } else if (data) {
            ForwardReceivedData(is_start, stream.current_group_id, stream.current_subgroup_id, data);

            // Buffer for cache/full parse
            stream.buffer.Push(*data);
        }

        auto& s_hdr = stream.buffer.GetAny<quicr::messages::StreamHeaderSubGroup>();

        while (true) {
            if (not stream.buffer.AnyHasValueB()) {
                stream.buffer.InitAnyB<quicr::messages::StreamSubGroupObject>();
            }

            auto& obj = stream.buffer.GetAnyB<quicr::messages::StreamSubGroupObject>();
            obj.properties.emplace(*s_hdr.properties);
            if (stream.buffer >> obj) {
                subscribe_track_metrics_.objects_received++;

                if (stream.next_object_id.has_value()) {
                    if (stream.current_group_id != s_hdr.group_id || stream.current_subgroup_id != s_hdr.subgroup_id) {
                        stream.next_object_id = obj.object_delta;
                    } else {
                        *stream.next_object_id += obj.object_delta;
                    }
                } else {
                    stream.next_object_id = obj.object_delta;
                }

                stream.current_group_id = s_hdr.group_id;
                stream.current_subgroup_id = s_hdr.subgroup_id.value();

                if (pending_new_group_request_id_.has_value() &&
                    (s_hdr.group_id == 0 || s_hdr.group_id > *pending_new_group_request_id_)) {
                    pending_new_group_request_id_.reset();
                }

                if (!s_hdr.subgroup_id.has_value()) {
                    if (obj.properties->subgroup_id_mode != quicr::messages::SubgroupIdType::kSetFromFirstObject) {
                        throw quicr::messages::ProtocolViolationException("Subgoup ID mismatch");
                    }
                    // Set the subgroup ID from the first object ID.
                    s_hdr.subgroup_id = stream.next_object_id;
                }

                ObjectReceived({ s_hdr.group_id,
                                 stream.next_object_id.value(),
                                 s_hdr.subgroup_id.value(),
                                 obj.payload.size(),
                                 obj.object_status,
                                 s_hdr.priority,
                                 std::nullopt,
                                 quicr::TrackMode::kStream,
                                 obj.extensions,
                                 obj.immutable_extensions },
                               obj.payload);

                *stream.next_object_id += 1;
                stream.buffer.ResetAnyB<quicr::messages::StreamSubGroupObject>();
            }

            break; // Not complete, wait for more data
        }
    }

    void SubscribeTrackHandler::DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data)
    {
        is_datagram_ = true;

        dgram_buffer_.Clear();
        dgram_buffer_.Push(*data);

        quicr::messages::ObjectDatagram msg;
        if (dgram_buffer_ >> msg) {
            ForwardReceivedData(false, msg.group_id, 0, data);

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
                                                    uint64_t group_id,
                                                    uint64_t subgroup_id,
                                                    std::shared_ptr<const std::vector<uint8_t>> data)
    {
        auto self_connection_handle = GetConnectionId();

        auto track_alias = GetTrackAlias();
        if (!track_alias.has_value()) {
            SPDLOG_DEBUG("Data without valid track alias");
            return;
        }

        peering::DataType d_type;

        if (is_datagram_) {
            d_type = peering::DataType::kDatagram;
        } else {
            d_type = peering::DataType::kExistingStream;

            if (is_new_stream) {
                d_type = peering::DataType::kNewStream;

                if (GetDeliveryTimeout().value_or(std::chrono::milliseconds(kDefaultObjectTtl)).count() == 0) {
                    // Use default if delivery timeout is not set
                    SetDeliveryTimeout(std::chrono::milliseconds(server_.config_.object_ttl_));
                }
            }
        }

        if (not is_from_peer_) {
            server_.peer_manager_.ClientDataRecv(*track_alias,
                                                 GetPriority(),
                                                 GetDeliveryTimeout().value_or(std::chrono::milliseconds(0)).count(),
                                                 d_type,
                                                 data);
        }

        // Fanout object to subscribe namespaces
        for (const auto [_, conn_subs] : sub_namespaces_) {
            for (const auto& [_, handler] : conn_subs) {
                handler->ForwardPublishedData(GetTrackAlias().value(), is_new_stream, group_id, subgroup_id, data);
            }
        }

        // Fanout object to subscribers
        for (auto& [conn_handle, pub_handler] : subscribers) {

            if (!pub_handler->SentFirstObject(group_id, subgroup_id)) {
                // Pipeline not enabled, use full object forwarding instead
                continue;
            }

            pub_handler->ForwardPublishedData(is_new_stream, group_id, subgroup_id, data);
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
                          server_.state_.pub_namespace_active[{ GetFullTrackName().name_space, GetConnectionId() }];
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