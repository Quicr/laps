#pragma once

#include "publish_namespace_handler.h"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace laps {
    /**
     * @brief Track ranking by property type, track alias and value of track
     */
    class TrackRanking
    {
      public:
        using PropertyType = uint64_t;
        using TrackAlias = uint64_t;
        using PropertyValue = uint64_t;
        struct TrackTickInfo
        {
            uint64_t insert_seq_num; // Track ranking update value sequence number
            uint64_t latest_tick;    // Most recent tick value for the track
        };
        using TrackEntry = std::unordered_map<TrackAlias, TrackTickInfo>; // Stores insert and latest ticks

        /**
         * @brief Update track ranking value for property type and track alias
         *
         * @param track_alias           Track alias to update
         * @param prop                  Property type value
         * @param value                 Value of the property
         * @param tick                  Current tick value
         */
        void UpdateValue(const TrackAlias track_alias,
                         const uint64_t prop,
                         const uint64_t value,
                         const uint64_t tick,
                         const uint64_t connection_id)
        {
            // Increment sequence number for this update
            ++update_value_seq_num_;

            // Check if track exists in a different value bucket
            bool needs_rebuild = false;
            bool value_decreased = false;
            for (auto it = ordered_tracks_.lower_bound({ prop, 0 });
                 it != ordered_tracks_.end() && it->first.first == prop;) {
                if (it->first.second != value && it->second.contains(track_alias)) {
                    if (it->first.second > value) {
                        value_decreased = true;
                    }

                    it->second.erase(track_alias); // Remove from old bucket
                    if (it->second.empty()) {
                        it = ordered_tracks_.erase(it); // erase returns next iterator
                        continue;
                    }
                    needs_rebuild = true;
                }
                ++it;
            }

            auto [prop_it, inserted] = ordered_tracks_.try_emplace({ prop, value });

            // Check if track is new to this bucket
            bool track_is_new = !prop_it->second.contains(track_alias);

            if (track_is_new) {
                auto seq_num = value_decreased ? (std::numeric_limits<uint64_t>::max() >> 1) - update_value_seq_num_
                                               : 1ULL << 63 | update_value_seq_num_;
                prop_it->second[track_alias] = {
                    seq_num, tick
                }; // insert_seq_num is current sequence, latest_tick set to current tick
            } else {
                prop_it->second[track_alias].latest_tick = tick; // Only update latest_tick for existing tracks
            }

            // Store connection ID for this track
            track_connections_[track_alias] = connection_id;

            // Rebuild if track moved buckets or new bucket was created
            needs_rebuild = needs_rebuild || inserted;

            SPDLOG_INFO("Update Value ta: {} prop: {} value: {} tick: {} conn_id: {}",
                        track_alias,
                        prop,
                        value,
                        tick,
                        connection_id);

            // Remove inactive tracks from ordered_tracks_
            for (auto it = ordered_tracks_.begin(); it != ordered_tracks_.end();) {
                auto& entry = it->second;
                for (auto track_it = entry.begin(); track_it != entry.end();) {
                    const auto& tick_info = track_it->second;
                    if (tick >= tick_info.latest_tick && tick - tick_info.latest_tick > inactive_age_ms_) {
                        track_it = entry.erase(track_it);
                        needs_rebuild = true;
                    } else {
                        ++track_it;
                    }
                }
                if (entry.empty()) {
                    it = ordered_tracks_.erase(it);
                } else {
                    ++it;
                }
            }

            if (needs_rebuild) {
                flat_track_list_.clear();

                auto first = ordered_tracks_.lower_bound({ prop, 0 });
                auto last = ordered_tracks_.upper_bound({ prop, std::numeric_limits<PropertyValue>::max() });

                for (auto it = std::make_reverse_iterator(last); it != std::make_reverse_iterator(first); ++it) {
                    auto& [key, entry] = *it;

                    std::vector<std::tuple<TrackAlias, uint64_t, uint64_t, uint64_t>> sort_tracks;
                    for (const auto& [ta, tick_info] : entry) {
                        sort_tracks.emplace_back(
                          ta, tick_info.insert_seq_num, tick_info.latest_tick, track_connections_[ta]);
                    }
                    std::ranges::sort(sort_tracks, [](const auto& a, const auto& b) {
                        if (std::get<1>(a) != std::get<1>(b))
                            return std::get<1>(a) < std::get<1>(b); // ascending insert_seq_num
                        return std::get<0>(a) > std::get<0>(b);
                    });

                    flat_track_list_.insert(flat_track_list_.end(), sort_tracks.begin(), sort_tracks.end());
                }

            } else {
                // Update latest_tick in flat_track_list_ in-place
                for (auto& [alias, insert_seq_num, latest_tick, conn_id] : flat_track_list_) {
                    if (alias == track_alias) {
                        latest_tick = tick;
                        conn_id = connection_id;
                        break;
                    }
                }
            }

            // notify each subscribe namespace (aka publish namespace handler)
            for (auto ns_it = ns_handlers_.begin(); ns_it != ns_handlers_.end();) {
                auto& [ns_hash, conn_handlers] = *ns_it;
                for (auto conn_it = conn_handlers.begin(); conn_it != conn_handlers.end();) {
                    if (auto h = conn_it->second.lock()) {
                        h->UpdateTrackRanking(flat_track_list_);
                        ++conn_it;
                    } else {
                        conn_it = conn_handlers.erase(conn_it); // returns next iterator
                    }
                }
                // Remove namespace entry if no handlers left
                if (conn_handlers.empty()) {
                    ns_it = ns_handlers_.erase(ns_it);
                } else {
                    ++ns_it;
                }
            }
        }

        /*
         * Getter/Setters
         */
        constexpr void SetMaxSelected(const uint64_t max) { max_tracks_selected_ = max; }
        constexpr uint64_t GetMaxSelected() { return max_tracks_selected_; }

        constexpr void SetInactiveAge(const uint64_t age_ms) { inactive_age_ms_ = age_ms; }
        constexpr uint64_t GetInactiveAge() { return inactive_age_ms_; }

        void AddNamespaceHandler(std::weak_ptr<PublishNamespaceHandler> handler)
        {
            if (auto h = handler.lock()) {
                auto th = quicr::TrackHash({ .name_space = h->GetPrefix(), .name = {} });
                ns_handlers_[th.track_namespace_hash].try_emplace(h->GetConnectionId(), handler);
            }
        }

        void RemoveNamespaceHandler(std::weak_ptr<PublishNamespaceHandler> handler)
        {
            if (auto h = handler.lock()) {
                auto th = quicr::TrackHash({ .name_space = h->GetPrefix(), .name = {} });
                ns_handlers_[th.track_namespace_hash].erase(h->GetConnectionId());
            }
        }

      private:
        uint64_t max_tracks_selected_{ 32 }; // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 1500 };   // Age in ms of a track that is considered stale/inactive
        uint64_t update_value_seq_num_{ 0 }; // Track ranking update value sequence number

        struct TrackInfo
        {
            uint64_t latest_value{ 0 };
            uint64_t latest_tick_ms{ 0 };
        };

        /**
         * @brief Ordered list of tracks.
         *
         * @details Key is a pair of PropertyType and PropertyValue. The value is a set of pairs. Each value pair
         *      conveys the latest tick value  and track alias.
         *      Sorting is performed by sorting both the map and the set.  The sort order is by property type,
         *      property value, and latest tick value.
         *
         */
        std::map<std::pair<PropertyType, PropertyValue>, TrackEntry> ordered_tracks_;
        std::vector<std::tuple<TrackAlias, uint64_t, uint64_t, uint64_t>>
          flat_track_list_;                                          // <alias, insert_seq_num, latest_tick, conn_id>
        std::unordered_map<TrackAlias, uint64_t> track_connections_; // Map track alias to connection ID

        /**
         * @brief Publish namespace handlers that are related to this track ranking
         * @details Indexed by namespace hash, then by connection ID
         */
        std::map<quicr::TrackNamespaceHash, std::map<uint64_t, std::weak_ptr<PublishNamespaceHandler>>> ns_handlers_;
    };
}
