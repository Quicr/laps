// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "publish_namespace_handler.h"

#include "publish_handler.h"
#include <unordered_set>

namespace laps {
    PublishNamespaceHandler::PublishNamespaceHandler(const quicr::TrackNamespace& prefix,
                                                     std::weak_ptr<quicr::TickService> tick_service)
      : quicr::PublishNamespaceHandler(prefix)
      , tick_service_(tick_service)
    {
    }
}

void
laps::PublishNamespaceHandler::PublishTrack(std::shared_ptr<quicr::PublishTrackHandler> handler)
{
    if (active_tracks_.size() < max_tracks_selected_) {
        quicr::PublishNamespaceHandler::PublishTrack(handler);

        quicr::TickService::TickType cur_ticks{ 0 };
        if (auto tick_svc = tick_service_.lock()) {
            cur_ticks = tick_svc->Milliseconds();
        }
        active_tracks_.emplace(handler->GetTrackAlias().value(), ActiveTrack{ cur_ticks, handler });
    }
}

quicr::PublishTrackHandler::PublishObjectStatus
laps::PublishNamespaceHandler::PublishObject(quicr::TrackFullNameHash track_full_name_hash,
                                             const quicr::ObjectHeaders& object_headers,
                                             quicr::BytesSpan data)
{
    // TODO: Implement subscribe namespace level per track filters
    //      Process incoming watched extensions to form Top-N

    if (const auto pub_it = handlers_.find(track_full_name_hash); pub_it != handlers_.end()) {

        auto handler = dynamic_pointer_cast<PublishTrackHandler>(pub_it->second);

        // Use pipeline forwarding now that first object has been sent
        if (handler->SentFirstObject(object_headers.group_id, object_headers.subgroup_id)) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
        }

        if (object_headers.track_mode.has_value()) {
            pub_it->second->SetDefaultTrackMode(object_headers.track_mode.value());
        }

        // Not using pipeline forwarding, publish complete object
        return pub_it->second->PublishObject(object_headers, data);
    }

    return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
}

quicr::PublishTrackHandler::PublishObjectStatus
laps::PublishNamespaceHandler::ForwardPublishedData(quicr::TrackFullNameHash track_full_name_hash,
                                                    bool is_new_stream,
                                                    uint64_t group_id,
                                                    uint64_t subgroup_id,
                                                    std::shared_ptr<const std::vector<uint8_t>> data)
{
    // TODO: Implement subscribe namespace level per track filters
    //      Process incoming watched extensions to form Top-N

    if (const auto pub_it = handlers_.find(track_full_name_hash); pub_it != handlers_.end()) {

        // Use pipeline forwarding now that first object has been sent
        auto handler = dynamic_pointer_cast<PublishTrackHandler>(pub_it->second);

        if (!handler->SentFirstObject(group_id, subgroup_id)) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
        }

        // Using pipeline forwarding
        return pub_it->second->ForwardPublishedData(is_new_stream, group_id, subgroup_id, data);
    }

    return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
}

void
laps::PublishNamespaceHandler::UpdateTrackRanking(
  std::span<const std::tuple<quicr::messages::TrackAlias, uint64_t, uint64_t>> ordered_tracks)
{
    uint64_t i = 0;
    std::unordered_map<uint64_t, uint64_t> updated_tracks;

    for (const auto& [ta, tick, publisher_conn_id] : ordered_tracks) {
        if (i++ >= max_tracks_selected_) {
            break;
        }

        // Filter out self-tracks
        if (publisher_conn_id == GetConnectionId()) {
            SPDLOG_INFO("Skipping self-track {} (connection_id: {})", ta, publisher_conn_id);
            continue;
        }

        SPDLOG_INFO("Update track tracking: Top track {} track alias: {} from conn {}", i, ta, publisher_conn_id);

        if (active_tracks_.contains(ta)) {
            continue;
        }

        auto track_it = handlers_.find(ta);
        if (track_it == handlers_.end()) {
            PublishTrack(track_it->second);
        } else {
            auto [_, is_new] = active_tracks_.try_emplace(ta, ActiveTrack{ tick, track_it->second });

            if (is_new) {
                SPDLOG_INFO("Publish track {} is newly selected", ta);
            }
        }

        updated_tracks.emplace(ta, tick);
    }

    if (active_tracks_.size() > max_tracks_selected_) {
        // Cleanup old tracks
        std::vector<uint64_t> rm_tracks;
        for (auto& [ta, track] : active_tracks_) {
            auto up_it = updated_tracks.find(ta);
            if (up_it != updated_tracks.end()) {
                // Update the latest tick so that it stays current
                track.latest_tick = up_it->second;
                continue;
            }

            // Only remove tracks after delay/grace period
            if (auto tick_svc = tick_service_.lock();
                tick_svc->Milliseconds() - track.latest_tick > delay_publish_done_ms_) {
                if (auto h = track.handler.lock()) {
                    quicr::PublishNamespaceHandler::UnPublishTrack(h);
                }

                rm_tracks.emplace_back(ta);
            }
        }

        for (auto& ta : rm_tracks) {
            active_tracks_.erase(ta);
        }
    }
}
