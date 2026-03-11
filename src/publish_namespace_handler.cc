// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "publish_namespace_handler.h"

#include "publish_handler.h"
#include <unordered_set>

namespace laps {
    PublishNamespaceHandler::PublishNamespaceHandler(const quicr::TrackNamespace& prefix)
      : quicr::PublishNamespaceHandler(prefix)
    {
    }
}

void
laps::PublishNamespaceHandler::PublishTrack(std::shared_ptr<quicr::PublishTrackHandler> handler)
{
    quicr::PublishNamespaceHandler::PublishTrack(handler);
    if (active_tracks_.size() < max_tracks_selected_) {
        active_tracks_.emplace(handler->GetTrackAlias().value(), handler);
    }
}

quicr::PublishTrackHandler::PublishObjectStatus
laps::PublishNamespaceHandler::PublishObject(quicr::TrackFullNameHash track_full_name_hash,
                                             const quicr::ObjectHeaders& object_headers,
                                             quicr::BytesSpan data)
{
    // TODO: Implement subscribe namespace level per track filters
    //      Process incoming watched extensions to form Top-N

    if (const auto pub_it = active_tracks_.find(track_full_name_hash); pub_it != active_tracks_.end()) {
        if (auto pub_handler = pub_it->second.lock()) {
            auto handler = dynamic_pointer_cast<PublishTrackHandler>(pub_handler);

            // Use pipeline forwarding now that first object has been sent
            if (handler->SentFirstObject(object_headers.group_id, object_headers.subgroup_id)) {
                return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
            }

            if (object_headers.track_mode.has_value()) {
                pub_handler->SetDefaultTrackMode(object_headers.track_mode.value());
            }

            // Not using pipeline forwarding, publish complete object
            return pub_handler->PublishObject(object_headers, data);
        }
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

    if (const auto pub_it = active_tracks_.find(track_full_name_hash); pub_it != active_tracks_.end()) {
        if (auto pub_handler = pub_it->second.lock()) {
            // Use pipeline forwarding now that first object has been sent
            auto handler = dynamic_pointer_cast<PublishTrackHandler>(pub_handler);

            if (!handler->SentFirstObject(group_id, subgroup_id)) {
                return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
            }

            // Using pipeline forwarding
            return pub_handler->ForwardPublishedData(is_new_stream, group_id, subgroup_id, data);
        }
    }

    return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
}

void
laps::PublishNamespaceHandler::UpdateTrackRanking(
  std::span<const std::pair<quicr::messages::TrackAlias, uint64_t>> ordered_tracks)
{
    // ordered_tracks is sorted by property value descending. Each entry is (TrackAlias, PropertyValue).
    // Incumbents (tracks already in active_tracks_) win ties: they can only be displaced by a track
    // with a strictly higher value.

    // Find the cutoff value: the value at position N in the ranked list.
    // All tracks with value > cutoff are unconditionally selected.
    // Tracks at the cutoff value are selected only if they are incumbents, or if slots remain.
    uint64_t cutoff_value = 0;
    uint64_t count = 0;
    for (const auto& [ta, value] : ordered_tracks) {
        cutoff_value = value;
        if (++count >= max_tracks_selected_) {
            break;
        }
    }

    std::unordered_set<uint64_t> new_active;

    // First pass: select all tracks strictly above cutoff, and retain incumbents at or above cutoff
    for (const auto& [ta, value] : ordered_tracks) {
        if (value < cutoff_value) {
            break;
        }

        if (value > cutoff_value) {
            // Strictly above cutoff — always selected
            new_active.emplace(ta);
            if (!active_tracks_.contains(ta)) {
                auto track_it = handlers_.find(ta);
                if (track_it != handlers_.end()) {
                    SPDLOG_INFO("Update track ranking: selecting new track alias: {} value: {}", ta, value);
                    active_tracks_.try_emplace(ta, track_it->second);
                }
            } else {
                SPDLOG_INFO("Update track ranking: retaining track alias: {} value: {}", ta, value);
            }
        } else if (active_tracks_.contains(ta)) {
            // At cutoff value — incumbent wins tie
            if (new_active.size() < max_tracks_selected_) {
                SPDLOG_INFO("Update track ranking: retaining incumbent track alias: {} value: {}", ta, value);
                new_active.emplace(ta);
            }
        }
    }

    // Second pass: fill remaining slots from tracks at cutoff value (non-incumbents)
    if (new_active.size() < max_tracks_selected_) {
        for (const auto& [ta, value] : ordered_tracks) {
            if (new_active.size() >= max_tracks_selected_) {
                break;
            }

            if (value < cutoff_value) {
                break;
            }

            if (value > cutoff_value || new_active.contains(ta)) {
                continue;
            }

            auto track_it = handlers_.find(ta);
            if (track_it == handlers_.end()) {
                SPDLOG_INFO("Skipping track {} due to missing handler", ta);
                continue;
            }

            SPDLOG_INFO("Update track ranking: selecting new track alias: {} value: {}", ta, value);
            new_active.emplace(ta);
            active_tracks_.try_emplace(ta, track_it->second);
        }
    }

    // Remove tracks no longer in the active set
    std::vector<uint64_t> rm_tracks;
    for (auto& [ta, handler] : active_tracks_) {
        if (!new_active.contains(ta)) {
            rm_tracks.emplace_back(ta);
        }
    }

    for (auto& ta : rm_tracks) {
        active_tracks_.erase(ta);
    }
}
