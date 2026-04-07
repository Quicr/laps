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
    auto [it, inserted] = published_tracks_.emplace(handler->GetTrackAlias().value(), ActiveTrack{ cur_ticks, handler });

    // TOPN_TRACE: Log when a track is published to this handler
    SPDLOG_INFO("TOPN_TRACE PUBLISH_TRACK my_conn:{} ta:{} inserted:{} total_pub_tracks:{}",
                GetConnectionId(),
                handler->GetTrackAlias().value(),
                inserted,
                published_tracks_.size());
}

quicr::PublishTrackHandler::PublishObjectStatus
laps::PublishNamespaceHandler::PublishObject(quicr::TrackFullNameHash track_full_name_hash,
                                             const quicr::ObjectHeaders& object_headers,
                                             quicr::BytesSpan data,
                                             std::optional<quicr::messages::StreamHeaderProperties> stream_mode)
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
        return pub_it->second->PublishObject(object_headers, data, stream_mode);
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
    // TOPN_TRACE: Log entry to UpdateTrackRanking
    SPDLOG_INFO("TOPN_TRACE HANDLER_UPDATE_START my_conn:{} prop_type:{} max_sel:{} pub_tracks_count:{} ordered_count:{}",
                GetConnectionId(),
                property_type_.value_or(0),
                max_tracks_selected_,
                published_tracks_.size(),
                ordered_tracks.size());

    if (!property_type_.has_value()) {
        SPDLOG_INFO("TOPN_TRACE HANDLER_SKIP_NO_PROP my_conn:{}", GetConnectionId());
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

    // TOPN_TRACE: Log published tracks before selection
    std::string pub_tracks_str;
    for (const auto& [ta, track] : published_tracks_) {
        auto status_str = "unknown";
        if (auto h = track.handler.lock()) {
            switch (h->GetStatus()) {
                case PublishTrackHandler::Status::kOk: status_str = "ok"; break;
                case PublishTrackHandler::Status::kPaused: status_str = "paused"; break;
                default: status_str = "other"; break;
            }
        }
        pub_tracks_str += fmt::format("[ta={},status={}]", ta, status_str);
    }
    SPDLOG_INFO("TOPN_TRACE HANDLER_PUB_TRACKS my_conn:{} tracks:{}", GetConnectionId(), pub_tracks_str);

    std::unordered_map<uint64_t, uint64_t> active_tracks;
    std::string selection_trace;
    for (const auto& [ta, insert_seq_num, latest_tick, publisher_conn_id] : ordered_tracks) {
        if (active_tracks.size() >= max_tracks_selected_) {
            selection_trace += fmt::format("[ta={},SKIP_MAX_REACHED]", ta);
            break;
        }

        // Filter out self-tracks
        if (publisher_conn_id == GetConnectionId()) {
            selection_trace += fmt::format("[ta={},SKIP_SELF,pub_conn={}]", ta, publisher_conn_id);
            SPDLOG_DEBUG("Skipping self-track {} (connection_id: {})", ta, publisher_conn_id);
            continue;
        }

        auto pub_track_it = published_tracks_.find(ta);
        if (pub_track_it == published_tracks_.end()) {
            selection_trace += fmt::format("[ta={},SKIP_NOT_PUBLISHED,pub_conn={}]", ta, publisher_conn_id);
            // Publish tracks should/must exists before this is call. They are managed by PublishTrack()
            SPDLOG_WARN("Track {} missing from publish_tracks; track alias: {} from conn {}",
                        active_tracks.size(),
                        ta,
                        publisher_conn_id);
            continue;
        }

        SPDLOG_DEBUG("Update track tracking: Top track {} track alias: {} from conn {}",
                     active_tracks.size(),
                     ta,
                     publisher_conn_id);

        if (auto h = pub_track_it->second.handler.lock(); h->GetStatus() == PublishTrackHandler::Status::kPaused) {
            selection_trace += fmt::format("[ta={},RESUME,pub_conn={}]", ta, publisher_conn_id);
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
        } else {
            selection_trace += fmt::format("[ta={},ALREADY_ACTIVE,pub_conn={}]", ta, publisher_conn_id);
        }

        active_tracks.emplace(ta, latest_tick);
    }

    // TOPN_TRACE: Log selection result
    std::string active_str;
    for (const auto& [ta, tick] : active_tracks) {
        active_str += fmt::format("[ta={}]", ta);
    }
    SPDLOG_INFO("TOPN_TRACE HANDLER_SELECTION my_conn:{} active_count:{} active:{} trace:{}",
                GetConnectionId(),
                active_tracks.size(),
                active_str,
                selection_trace);

    if (published_tracks_.size() > max_tracks_selected_) {
        // Unpublish tracks that are too old
        std::string pause_trace;
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
                    pause_trace += fmt::format("[ta={},PAUSING]", ta);
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
        if (!pause_trace.empty()) {
            SPDLOG_INFO("TOPN_TRACE HANDLER_PAUSE my_conn:{} pause_trace:{}", GetConnectionId(), pause_trace);
        }
    } else {
        SPDLOG_INFO("TOPN_TRACE HANDLER_NO_PAUSE my_conn:{} pub_tracks_size:{} max_sel:{} (condition: {} > {} = false)",
                    GetConnectionId(),
                    published_tracks_.size(),
                    max_tracks_selected_,
                    published_tracks_.size(),
                    max_tracks_selected_);
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