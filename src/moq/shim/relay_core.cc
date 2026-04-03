// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "relay_core.h"

#include "subscribe_handler.h"

#include <quicr/detail/uintvar.h>

#include <spdlog/spdlog.h>

using namespace laps;

namespace laps::moq::shim {

    void RelayCore::ProcessSubscribe(const AnnouncerSubscribeHandlerFactory& announcer_subscribe_handler_factory,
                                     quicr::ConnectionHandle connection_handle,
                                     uint64_t request_id,
                                     const quicr::TrackHash& th,
                                     const quicr::FullTrackName& track_full_name,
                                     const quicr::messages::SubscribeAttributes& attrs,
                                     std::optional<quicr::messages::Location> largest)
    {
        const auto& start_location = attrs.start_location;

        if (largest.has_value()) {
            SPDLOG_LOGGER_INFO(LOGGER, "Subscribe largest group: {} object: {}", largest->group, largest->object);
        }

        if (connection_handle == 0 && request_id == 0) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Processing peer subscribe track alias: {} priority: {} new_group_request: {}",
                                th.track_fullname_hash,
                                attrs.priority,
                                attrs.new_group_request_id.has_value() ? *attrs.new_group_request_id : -1);
        } else {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "Processing subscribe connection handle: {} request_id: {} track alias: {} priority: "
                               "{} ns: {} name: {} new_group_request: {} start group: {} object: {}",
                               connection_handle,
                               request_id,
                               th.track_fullname_hash,
                               attrs.priority,
                               th.track_namespace_hash,
                               th.track_name_hash,
                               attrs.new_group_request_id.has_value() ? *attrs.new_group_request_id : -1,
                               start_location.group,
                               start_location.object);

            state_.subscribe_active_[{ track_full_name.name_space, th.track_name_hash }].emplace(
              State::SubscribeInfo{ connection_handle,
                                    request_id,
                                    th.track_fullname_hash,
                                    attrs.priority,
                                    attrs.delivery_timeout,
                                    attrs.start_location });
            state_.subscribe_alias_req_id[{ connection_handle, request_id }] = th.track_fullname_hash;

            const auto subscribes_emplace = state_.subscribes.try_emplace(
              { th.track_fullname_hash, connection_handle },
              State::SubscribePublishHandlerInfo{ track_full_name,
                                                  th.track_fullname_hash,
                                                  request_id,
                                                  attrs.priority,
                                                  static_cast<uint32_t>(attrs.delivery_timeout.count()),
                                                  attrs.group_order,
                                                  {} });

            if (subscribes_emplace.second) {
                auto subscribe_params = quicr::messages::Parameters{}
                                          .Add(quicr::messages::ParameterType::kSubscriberPriority, attrs.priority)
                                          .Add(quicr::messages::ParameterType::kGroupOrder, attrs.group_order);

                quicr::messages::Subscribe subscribe_msg(request_id,
                                                         track_full_name.name_space,
                                                         track_full_name.name,
                                                         subscribe_params);

                quicr::Bytes subscribe_payload;
                subscribe_payload << subscribe_msg;

                const auto message_type_size =
                  quicr::UintVar::Size(*quicr::UintVar(subscribe_payload).begin());
                subscribe_payload.erase(subscribe_payload.begin(),
                                        subscribe_payload.begin() + message_type_size + sizeof(uint16_t));

                peer_manager_.ClientSubscribe(track_full_name, attrs, subscribe_payload);
            }
        }

        for (auto pub_sub_it = state_.pub_subscribes.lower_bound({ th.track_fullname_hash, 0 });
             pub_sub_it != state_.pub_subscribes.end();
             ++pub_sub_it) {
            if (pub_sub_it->first.first != th.track_fullname_hash) {
                break;
            }
            if (pub_sub_it->second->IsPublisherInitiated()) {
                pub_sub_it->second->Resume();
            }

            pub_sub_it->second->AddSubscriber(
              connection_handle, request_id, attrs.priority, attrs.delivery_timeout, attrs.start_location);

            DampenOrUpdateTrackSubscription(pub_sub_it->second, attrs.new_group_request_id.has_value());
        }

        for (auto& [announcer_key, track_alias_set] : state_.pub_namespace_active) {
            if (!announcer_key.first.HasSamePrefix(track_full_name.name_space) || !announcer_key.second) {
                continue;
            }

            auto existing_pub_sub_it = state_.pub_subscribes.find({ th.track_fullname_hash, announcer_key.second });
            if (existing_pub_sub_it == state_.pub_subscribes.end()) {
                if (!announcer_subscribe_handler_factory) {
                    SPDLOG_LOGGER_WARN(
                      LOGGER,
                      "Announcer subscribe skipped (no handler factory): announcer_handle={} track_alias={}",
                      announcer_key.second,
                      th.track_fullname_hash);
                    continue;
                }
                auto sub_track_handler = announcer_subscribe_handler_factory(
                  track_full_name,
                  static_cast<quicr::messages::ObjectPriority>(0),
                  quicr::messages::GroupOrder::kAscending);
                if (!sub_track_handler) {
                    SPDLOG_LOGGER_WARN(
                      LOGGER,
                      "Announcer subscribe skipped (factory returned null): announcer_handle={} track_alias={}",
                      announcer_key.second,
                      th.track_fullname_hash);
                    continue;
                }

                SPDLOG_LOGGER_INFO(LOGGER,
                                   "Sending subscribe to announcer connection handle: {0} subscribe track_alias: {1}",
                                   announcer_key.second,
                                   th.track_fullname_hash);

                track_alias_set.insert(th.track_fullname_hash);
                port_->SubscribeTrack(announcer_key.second, sub_track_handler);

                sub_track_handler->AddSubscriber(
                  connection_handle, request_id, attrs.priority, attrs.delivery_timeout, attrs.start_location);

                state_.pub_subscribes_by_req_id[{ sub_track_handler->GetRequestId().value(), announcer_key.second }] =
                  sub_track_handler;
                state_.pub_subscribes[{ th.track_fullname_hash, announcer_key.second }] = sub_track_handler;

                if (attrs.new_group_request_id) {
                    if (sub_track_handler->HasQuicrTransport()) {
                        sub_track_handler->RequestNewGroup();
                    } else {
                        sub_track_handler->SetNewGroupRequestId(0);
                        port_->UpdateTrackSubscription(
                          announcer_key.second,
                          std::static_pointer_cast<quicr::SubscribeTrackHandler>(sub_track_handler));
                    }
                }

            } else {
                auto dampen_target_it = state_.pub_subscribes.find({ th.track_fullname_hash, announcer_key.second });
                if (dampen_target_it != state_.pub_subscribes.end()) {
                    DampenOrUpdateTrackSubscription(dampen_target_it->second,
                                                      attrs.new_group_request_id.has_value());
                }
            }
        }
    }

    bool RelayCore::DampenOrUpdateTrackSubscription(
      std::shared_ptr<laps::SubscribeTrackHandler> sub_to_pub_track_handler,
      bool new_group_request)
    {
        if (sub_to_pub_track_handler->GetConnectionId() <= 1) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();

        uint64_t elapsed_ms = config_.sub_dampen_ms_ + 1;

        if (sub_to_pub_track_handler->pub_last_update_info_.time.has_value()) {
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - *sub_to_pub_track_handler->pub_last_update_info_.time)
                           .count();
        }

        if (new_group_request || elapsed_ms > config_.sub_dampen_ms_) {
            SPDLOG_LOGGER_INFO(LOGGER,
                               "Sending subscribe-update to publisher connection handler: {} subscribe "
                               "track_alias: {} new_group: {} pending_new_group_id: {}",
                               sub_to_pub_track_handler->GetConnectionId(),
                               sub_to_pub_track_handler->GetTrackAlias().value(),
                               new_group_request,
                               sub_to_pub_track_handler->GetPendingNewRquestId().has_value()
                                 ? *sub_to_pub_track_handler->GetPendingNewRquestId()
                                 : 0);

            sub_to_pub_track_handler->pub_last_update_info_.time = now;

            if (new_group_request) {
                if (sub_to_pub_track_handler->HasQuicrTransport()) {
                    sub_to_pub_track_handler->RequestNewGroup();
                } else {
                    sub_to_pub_track_handler->SetNewGroupRequestId(0);
                    port_->UpdateTrackSubscription(sub_to_pub_track_handler->GetConnectionId(),
                                                   std::static_pointer_cast<quicr::SubscribeTrackHandler>(
                                                     sub_to_pub_track_handler));
                }
            } else {
                port_->UpdateTrackSubscription(sub_to_pub_track_handler->GetConnectionId(),
                                             std::static_pointer_cast<quicr::SubscribeTrackHandler>(
                                               sub_to_pub_track_handler));
            }
        }

        return false;
    }

} // namespace laps::moq::shim
