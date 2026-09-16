// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_handler.h"
#include "config.h"
#include "publish_handler.h"
#include "publish_namespace_handler.h"

#include <quicr/handlers/subscribe_track_handler.h>
#include <quicr/session.h>

namespace laps {
    SubscribeTrackHandler::SubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                                 std::uint8_t priority,
                                                 std::optional<quicr::messages::GroupOrder> group_order,
                                                 ClientManager& server,
                                                 std::weak_ptr<timeq::tick_service> tick_service,
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
        for (auto& [conn_handle, handler] : subscribers_) {
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

        auto prop = handler->GetPropertyType();
        if (!tracked_properties_value_.contains(prop)) {
            SPDLOG_INFO("Subscribe handler tracking property_type={} from namespace handler", prop);
            tracked_properties_value_.emplace(prop, PublishNamespaceHandler::TrackPropertyValue{});
        }

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

    void SubscribeTrackHandler::RemoveFromTrackRanking()
    {
        if (auto ranking = track_ranking_.lock()) {
            ranking->RemoveTrack(GetTrackAlias().value());
        }
    }

    void SubscribeTrackHandler::AddSubscriber(std::uint64_t conn_handle,
                                              std::uint64_t request_id,
                                              uint8_t priority,
                                              std::chrono::milliseconds delivery_timeout,
                                              quicr::messages::Location start_location)
    {
        if (subscribers_.contains(conn_handle)) {
            // Duplicate
            return;
        }

        if (conn_handle) {
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

            subscribers_.emplace(conn_handle, pub_track_h);
        } else {
            subscribers_.emplace(conn_handle, nullptr);
        }

        Resume();
    }

    void SubscribeTrackHandler::RemoveSubscriber(std::uint64_t conn_handle)
    {
        auto it = subscribers_.find(conn_handle);
        if (it != subscribers_.end()) {
            if (it->second) {
                server_.UnbindPublisherTrack(conn_handle, GetConnectionId(), it->second);
            }

            subscribers_.erase(conn_handle);
        }

        if (subscribers_.empty() && sub_namespaces_.empty()) {
            Pause();
        }
    }

    void SubscribeTrackHandler::UpdateTrackedProperties(std::optional<quicr::Extensions> extensions,
                                                        std::optional<quicr::Extensions> immutable_extensions)
    {
        auto update = [ta = GetTrackAlias().value(),
                       conn_id = GetConnectionId(),
                       ticks = tick_service_.lock(),
                       ranking = track_ranking_.lock()](
                        uint64_t prop, PublishNamespaceHandler::TrackPropertyValue& value, uint64_t recv_value) {
            timeq::tick_service::tick_type cur_tick{ 0 };
            if (ticks != nullptr) {
                cur_tick =
                  static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(ticks->get()).count());
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

    void SubscribeTrackHandler::ObjectReceived(const quicr::ObjectHeaders& object_headers,
                                               quicr::BytesSpan data,
                                               std::optional<quicr::messages::StreamHeaderProperties> stream_mode)
    {
        if (object_headers.track_mode.has_value()) {
            is_datagram_ = *object_headers.track_mode == quicr::TrackMode::kDatagram;
        }

        if (!GetTrackAlias().has_value()) {
            return;
        }

        UpdateTrackedProperties(object_headers.extensions, object_headers.immutable_extensions);

        if (pending_new_group_request_id_.has_value() &&
            (object_headers.group_id == 0 || object_headers.group_id > *pending_new_group_request_id_)) {
            pending_new_group_request_id_.reset();
        }

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (!server_.config_.disable_cache && GetTrackAlias().has_value()) {
            if (server_.cache_.count(GetTrackAlias().value()) == 0) {
                server_.cache_.insert(
                  std::make_pair(GetTrackAlias().value(),
                                 quicr::Cache<std::uint64_t, std::set<CacheObject>>{
                                   server_.cache_duration_ms_, 1000, server_.config_.tick_service_ }));
            }

            auto& cache_entry = server_.cache_.at(GetTrackAlias().value());

            if (auto group = cache_entry.Get(object_headers.group_id)) {
                group->insert(std::move(object));
            } else {
                cache_entry.Insert(object_headers.group_id, { std::move(object) }, server_.cache_duration_ms_);
            }
        }

        try {
            for (const auto& [_, conn_subs] : sub_namespaces_) {
                for (const auto& [_, handler] : conn_subs) {
                    handler->PublishObject(GetTrackAlias().value(), object_headers, data, stream_mode);
                }
            }

            for (auto& [conn_handle, pub_handler] : subscribers_) {
                if (conn_handle == 0) {
                    continue;
                }

                pub_handler->SetDefaultTrackMode(is_datagram_ ? quicr::TrackMode::kDatagram
                                                              : quicr::TrackMode::kStream);

                if (object_headers.group_id >= pub_handler->start_location_.group &&
                    object_headers.object_id >= pub_handler->start_location_.object) {
                    pub_handler->PublishObject(object_headers, data, stream_mode);
                }
            }

            if (!is_from_peer_ && subscribers_.contains(0)) {
                const auto ttl =
                  GetDeliveryTimeout().value_or(std::chrono::milliseconds(server_.config_.object_ttl_)).count();
                server_.SendObjectToPeers(
                  GetTrackAlias().value(), GetPriority(), ttl, is_datagram_, object_headers, data, stream_mode);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Caught exception trying to publish. (error={})", e.what());
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
            // The alias is assigned when the handler is bound to its connection, which is after a
            // publisher-initiated subscribe is accepted, so it can still be unset here
            SPDLOG_DEBUG("Track alias: {} subscribe status change reason: {} status: {}",
                         GetTrackAlias().value_or(0),
                         reason,
                         static_cast<int>(status));
        }
    }

    void SubscribeTrackHandler::SubgroupEnded(std::uint64_t group_id, std::uint64_t subgroup_id, bool reset)
    {
        if (!is_from_peer_ && GetTrackAlias().has_value() && subscribers_.contains(0)) {
            server_.PeerSubgroupEnded(GetTrackAlias().value(), group_id, subgroup_id, reset);
        }

        for (auto& [conn_handle, pub_handler] : subscribers_) {
            if (conn_handle == 0) {
                continue;
            }

            pub_handler->EndSubgroup(group_id, subgroup_id, !reset);
        }

        for (const auto& [_, conn_subs] : sub_namespaces_) {
            for (const auto& [_, handler] : conn_subs) {
                handler->EndSubgroup(group_id, subgroup_id, !reset);
            }
        }
    }

    void SubscribeTrackHandler::SetFromPeer()
    {
        is_from_peer_ = true;
    }

    void SubscribeTrackHandler::MetricsSampled(const quicr::SubscribeTrackMetrics& metrics)
    {
        const auto tfn = GetFullTrackName();
        server_.metrics_publisher_.QueueSubscribeMetrics(GetConnectionId(), tfn, SubscriberCount(), metrics);
    }
}
