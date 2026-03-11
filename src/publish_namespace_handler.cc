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
    all_handlers_.try_emplace(handler->GetTrackAlias().value(), handler);
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
    // A selected track stays selected unless:
    //   1. A non-selected track has a strictly higher value, displacing it from the top N
    //   2. MaxTimeSelected expires (handled upstream by excluding stale tracks from the list)

    // Build a map of current values for active tracks from the ranked list
    std::unordered_map<uint64_t, uint64_t> active_values; // ta -> value
    for (const auto& [ta, value] : ordered_tracks) {
        if (active_tracks_.contains(ta)) {
            active_values.emplace(ta, value);
        }
    }

    // Activate a track: move from deselected if present, otherwise from all_handlers_
    auto activate_track = [&](uint64_t ta, uint64_t value) {
        if (active_tracks_.contains(ta)) {
            return true;
        }

        // Check deselected list first (no re-publish needed)
        auto desel_it = deselected_tracks_.find(ta);
        if (desel_it != deselected_tracks_.end()) {
            SPDLOG_INFO("Update track ranking: reselecting track alias: {} value: {} from deselected", ta, value);
            active_tracks_.try_emplace(ta, desel_it->second);
            deselected_tracks_.erase(desel_it);
            std::erase(deselected_order_, ta);
            return true;
        }

        // Not in deselected — check all_handlers_ and re-publish if needed
        auto track_it = all_handlers_.find(ta);
        if (track_it == all_handlers_.end()) {
            SPDLOG_INFO("Skipping track {} due to missing handler", ta);
            return false;
        }

        // If not in base handlers_, track was dropped (PUBLISH_DONE sent) — re-publish
        if (!handlers_.contains(ta)) {
            SPDLOG_INFO("Update track ranking: re-publishing dropped track alias: {} value: {}", ta, value);
            quicr::PublishNamespaceHandler::PublishTrack(track_it->second);
        } else {
            SPDLOG_INFO("Update track ranking: selecting new track alias: {} value: {}", ta, value);
        }

        active_tracks_.try_emplace(ta, track_it->second);
        return true;
    };

    // Find the lowest value among active tracks
    auto find_lowest_active = [&]() -> std::pair<uint64_t, uint64_t> { // (ta, value)
        uint64_t lowest_ta = 0;
        uint64_t lowest_value = UINT64_MAX;
        for (const auto& [ta, value] : active_values) {
            if (value < lowest_value) {
                lowest_value = value;
                lowest_ta = ta;
            }
        }
        return { lowest_ta, lowest_value };
    };

    // Walk the ranked list (highest value first). Non-active tracks displace the
    // lowest active track only if they have a strictly higher value.
    for (const auto& [ta, value] : ordered_tracks) {
        if (active_tracks_.contains(ta)) {
            continue; // Already active, skip
        }

        if (active_values.size() < max_tracks_selected_) {
            // Empty slots — fill without needing to displace
            if (activate_track(ta, value)) {
                active_values.emplace(ta, value);
                SPDLOG_INFO("Update track ranking: filled empty slot with track alias: {} value: {}", ta, value);
            }
            continue;
        }

        auto [lowest_ta, lowest_value] = find_lowest_active();

        if (value <= lowest_value) {
            break; // No more displacements possible (list is sorted descending)
        }

        // Strictly higher value — displace the lowest active track
        SPDLOG_INFO("Update track ranking: track alias: {} value: {} displaces track alias: {} value: {}",
                     ta, value, lowest_ta, lowest_value);

        // Deselect the displaced track
        auto displaced_it = active_tracks_.find(lowest_ta);
        if (displaced_it != active_tracks_.end()) {
            if (!deselected_tracks_.contains(lowest_ta)) {
                SPDLOG_INFO("Update track ranking: deselecting track alias: {}", lowest_ta);
                deselected_tracks_.emplace(lowest_ta, displaced_it->second);
                deselected_order_.push_back(lowest_ta);
            }
            active_tracks_.erase(displaced_it);
        }
        active_values.erase(lowest_ta);

        // Activate the new track
        if (activate_track(ta, value)) {
            active_values.emplace(ta, value);
        }
    }

    // Remove any active tracks that were excluded from the ranked list entirely
    // (e.g. exceeded MaxTimeSelected)
    std::vector<uint64_t> rm_tracks;
    for (auto& [ta, handler] : active_tracks_) {
        if (!active_values.contains(ta)) {
            rm_tracks.emplace_back(ta);
            if (!deselected_tracks_.contains(ta)) {
                SPDLOG_INFO("Update track ranking: deselecting stale track alias: {}", ta);
                deselected_tracks_.emplace(ta, handler);
                deselected_order_.push_back(ta);
            }
        }
    }

    for (auto& ta : rm_tracks) {
        active_tracks_.erase(ta);
    }

    // Trim deselected list — drop oldest entries beyond max, sending PUBLISH_DONE
    while (deselected_order_.size() > max_tracks_deselected_) {
        auto oldest_ta = deselected_order_.front();
        deselected_order_.pop_front();

        auto it = deselected_tracks_.find(oldest_ta);
        if (it != deselected_tracks_.end()) {
            if (auto h = it->second.lock()) {
                SPDLOG_INFO("Update track ranking: dropping track alias: {} from deselected list, sending PUBLISH_DONE",
                            oldest_ta);
                quicr::PublishNamespaceHandler::UnPublishTrack(h);
            }
            deselected_tracks_.erase(it);
        }
    }
}
