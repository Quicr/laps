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
    quicr::PublishNamespaceHandler::PublishTrack(handler);

    quicr::TickService::TickType cur_ticks{ 0 };
    if (auto tick_svc = tick_service_.lock()) {
        cur_ticks = tick_svc->Milliseconds();
    }
    published_tracks_.emplace(handler->GetTrackAlias().value(), ActiveTrack{ cur_ticks, handler });

    if (property_type_.has_value()) {
        PublishTrack(handler);
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
  std::span<const std::tuple<quicr::messages::TrackAlias, uint64_t, uint64_t, uint64_t>> ordered_tracks)
{
    if (!property_type_.has_value()) {
        return;
    }

    // Update latest tick for each publish track
    for (auto& [ta, insert_seq_num, latest_tick, conn_id] : ordered_tracks) {
        SPDLOG_DEBUG("DEBUG: conn_id: {} ta: {} insert_seq_num: {} latest_tick: {}",
                     GetConnectionId(),
                     ta,
                     insert_seq_num,
                     latest_tick);

        auto pub_it = published_tracks_.find(ta);
        if (pub_it != published_tracks_.end()) {
            pub_it->second.last_updated_tick = latest_tick;
        }
    }

    std::unordered_map<uint64_t, uint64_t> active_tracks;
    for (const auto& [ta, insert_seq_num, latest_tick, publisher_conn_id] : ordered_tracks) {
        if (active_tracks.size() >= max_tracks_selected_) {
            break;
        }

        // Filter out self-tracks
        if (publisher_conn_id == GetConnectionId()) {
            SPDLOG_DEBUG("Skipping self-track {} (connection_id: {})", ta, publisher_conn_id);
            continue;
        }

        SPDLOG_DEBUG("Update track tracking: Top track {} track alias: {} from conn {}",
                     active_tracks.size(),
                     ta,
                     publisher_conn_id);

        auto pub_track_it = published_tracks_.find(ta);
        if (pub_track_it == published_tracks_.end()) {
            // Publish tracks should/must exists before this is call. They are managed by PublishTrack()
            SPDLOG_WARN("Track {} missing from publish_tracks; track alias: {} from conn {}",
                        active_tracks.size(),
                        ta,
                        publisher_conn_id);
            continue;
        }

        if (auto h = pub_track_it->second.handler.lock(); h->GetStatus() == PublishTrackHandler::Status::kPaused) {
            h->SetStatus(PublishTrackHandler::Status::kOk);

            auto track_it = handlers_.find(ta);
            if (track_it == handlers_.end()) {
                SPDLOG_DEBUG("Track {} is newly selected and will undergo PUBLISH flow track alias: {} conn_id: {}",
                             active_tracks.size(),
                             ta,
                             publisher_conn_id);
                PublishTrack(h);
            } else {
                pub_track_it->second.last_updated_tick = latest_tick;
            }
        }

        active_tracks.emplace(ta, latest_tick);
    }

    if (published_tracks_.size() > max_tracks_selected_) {
        // Unpublish tracks that are too old
        for (auto& [ta, track] : published_tracks_) {
            if (active_tracks.contains(ta)) {
                // Skip, track is active
                continue;
            }

            quicr::TickService::TickType cur_tick{ 0 };
            if (auto tick_svc = tick_service_.lock()) {
                cur_tick = tick_svc->Milliseconds();
            }

            if (auto h = track.handler.lock()) {
                if (h->GetStatus() == PublishTrackHandler::Status::kOk) {
                    SPDLOG_INFO("Setting track to Paused, no longer in top-n alias: {} conn_id: {} ticks: {} < {}",
                                ta,
                                GetConnectionId(),
                                track.last_updated_tick,
                                cur_tick);

                    if (auto hh = std::dynamic_pointer_cast<PublishTrackHandler>(h)) {
                        hh->AbruptCloseAllSubgroups();
                    }
                    h->SetStatus(PublishTrackHandler::Status::kPaused);
                }

                if (cur_tick - track.last_updated_tick > delay_publish_done_ms_) {
                    SPDLOG_INFO("Unpublish track, not in top-n track alias: {} conn_id: {} ticks: {} < {}",
                                ta,
                                GetConnectionId(),
                                track.last_updated_tick,
                                cur_tick);
                    // UnPublishTrack(h);
                }
            }
        }
    }
}

void
laps::PublishNamespaceHandler::EndSubgroup(uint64_t group_id, uint64_t subgroup_id, bool completed)
{
    for (auto& [ta, track] : published_tracks_) {
        if (auto handler = track.handler.lock()) {
            handler->EndSubgroup(group_id, subgroup_id, completed);
        }
    }
}