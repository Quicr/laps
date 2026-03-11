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
        using TrackEntry = std::unordered_map<TrackAlias, uint64_t>; // Value is the tick value of last update

        /**
         * @brief Update track ranking value for property type and track alias
         *
         * @param track_alias           Track alias to update
         * @param prop                  Property type value
         * @param value                 Value of the property
         * @param tick                  Current tick value
         */
        void UpdateValue(const TrackAlias track_alias, const uint64_t prop, const uint64_t value, const uint64_t tick)
        {
            // Check if track exists in a different value bucket
            bool needs_rebuild = false;
            for (auto it = ordered_tracks_.lower_bound({ prop, 0 });
                 it != ordered_tracks_.end() && it->first.first == prop;
                 ++it) {
                if (it->first.second != value && it->second.contains(track_alias)) {
                    it->second.erase(track_alias); // Remove from old bucket
                    if (it->second.empty()) {
                        it = ordered_tracks_.erase(it); // erase returns next iterator
                    }
                    needs_rebuild = true;
                    break;
                }
            }

            auto [prop_it, inserted] = ordered_tracks_.try_emplace({ prop, value });
            prop_it->second.insert_or_assign(track_alias, tick);

            // Rebuild if track moved buckets or new bucket was created
            needs_rebuild = needs_rebuild || inserted;

            SPDLOG_INFO("Update Value ta: {} prop: {} value: {} tick: {}", track_alias, prop, value, tick);

            if (needs_rebuild) {
                flat_track_list_.clear();
                auto rbegin = std::make_reverse_iterator(ordered_tracks_.lower_bound({ prop + 1, 0 }));
                auto rend = std::make_reverse_iterator(ordered_tracks_.lower_bound({ prop, 0 }));
                for (auto it = rbegin; it != rend; ++it) {
                    auto& [key, entry] = *it;

                    std::vector<std::pair<TrackAlias, uint64_t>> sort_tracks(entry.begin(), entry.end());
                    std::ranges::sort(sort_tracks, [](const auto& a, const auto& b) {
                        if (a.second != b.second)
                            return a.second < b.second; // by tick
                        return a.first < b.first;       // tie-break
                    });

                    flat_track_list_.insert(flat_track_list_.end(), sort_tracks.begin(), sort_tracks.end());
                }
            } else {
                // Update tick in flat_track_list_ in-place
                for (auto& [alias, tick_val] : flat_track_list_) {
                    if (alias == track_alias) {
                        tick_val = tick;
                        break;
                    }
                }
            }

            // notify each subscribe namespace (aka publish namespace handler)
            for (auto it = ns_handlers_.begin(); it != ns_handlers_.end();) {
                if (auto h = it->second.lock()) {
                    h->UpdateTrackRanking(flat_track_list_);
                    ++it;
                } else {
                    it = ns_handlers_.erase(it); // returns next iterator
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
                ns_handlers_.try_emplace(th.track_namespace_hash, handler);
            }
        }

        void RemoveNamespaceHandler(std::weak_ptr<PublishNamespaceHandler> handler)
        {
            if (auto h = handler.lock()) {
                auto th = quicr::TrackHash({ .name_space = h->GetPrefix(), .name = {} });
                ns_handlers_.erase(th.track_namespace_hash);
            }
        }

      private:
        uint64_t max_tracks_selected_{ 32 }; // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 5000 };   // Age in ms of a track that is considered stale/inactive

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
        std::vector<std::pair<TrackAlias, uint64_t>> flat_track_list_;

        /**
         * @brief Publish namespace handlers that are related to this track ranking
         */
        std::map<quicr::TrackNamespaceHash, std::weak_ptr<PublishNamespaceHandler>> ns_handlers_;
    };
}
