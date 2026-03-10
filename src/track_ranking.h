#pragma once

#include <algorithm>
#include <map>
#include <span>
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
        using TrackSelectedEntry = std::pair<TrackAlias, PropertyValue>;

        void UpdateValue(const TrackAlias track_alias, const uint64_t prop, const uint64_t value, const uint64_t tick)
        {
            auto [prop_it, _] = tracks_.try_emplace(prop);

            TrackInfo track_info{ .latest_value = value, .latest_tick_ms = tick };
            auto [t_it, is_new] = prop_it->second.try_emplace(track_alias, std::move(track_info));

            if (!is_new) {
                if (t_it->second.latest_value < value) {
                    SPDLOG_INFO("Increase in value TA: {} type: {} from: {} to: {} tick: {}",
                                track_alias,
                                prop,
                                t_it->second.latest_value,
                                value,
                                tick);
                } else if (t_it->second.latest_value > value) {
                    SPDLOG_INFO("Decrease in value TA: {} type: {} from: {} to: {} tick: {}",
                                track_alias,
                                prop,
                                t_it->second.latest_value,
                                value,
                                tick);
                }

                // Update
                t_it->second.latest_value = value;
                t_it->second.latest_tick_ms = tick;
            }

            ComputeSelected();
        }

        /*
         * Getter/Setters
         */
        constexpr void SetMaxSelected(const uint64_t max) { max_tracks_selected_ = max; }
        constexpr uint64_t GetMaxSelected() { return max_tracks_selected_; }

        constexpr void SetInactiveAge(const uint64_t age_ms) { inactive_age_ms_ = age_ms; }
        constexpr uint64_t GetInactiveAge() { return inactive_age_ms_; }

        /**
         * @brief Get the selected (aka ordered by best) list of track aliases
         *
         * @details Returns a list of track aliases and the property value, ordered by highest
         *      value first. Limit is used to limit the result set.
         *
         * @param limit           Limit the return set of selected, ordered by best first
         *
         * @returns Returns a span of pairs <TrackAlias, PropertyValue>
         */
        std::span<const TrackSelectedEntry> GetSelected(uint64_t limit)
        {
            if (limit == 0 || limit >= selected_tracks_.size()) {
                return selected_tracks_;
            }
            return std::span<const TrackSelectedEntry>(selected_tracks_.data(), limit);
        }

        /**
         * @brief Check if a track alias is in the top-N selected set
         *
         * @details Returns true if ranking data has not been populated yet (no filtering)
         *          or if the track alias is within the top max_tracks_selected_ tracks.
         */
        bool IsSelected(TrackAlias track_alias) const
        {
            if (selected_tracks_.empty()) {
                return true;
            }

            return std::any_of(selected_tracks_.begin(), selected_tracks_.end(),
                               [track_alias](const auto& entry) { return entry.first == track_alias; });
        }

      private:
        uint64_t max_tracks_selected_{ 1 }; // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 5000 };   // Age in ms of a track that is considered stale/inactive

        /**
         * @brief Recompute the selected top-N tracks from current ranking data
         */
        void ComputeSelected()
        {
            // Collect best value per track alias across all property types
            std::unordered_map<TrackAlias, PropertyValue> best_values;
            for (const auto& [prop, tracks] : tracks_) {
                for (const auto& [ta, info] : tracks) {
                    auto [it, is_new] = best_values.try_emplace(ta, info.latest_value);
                    if (!is_new && info.latest_value > it->second) {
                        it->second = info.latest_value;
                    }
                }
            }

            selected_tracks_.clear();
            selected_tracks_.reserve(best_values.size());
            for (const auto& [ta, val] : best_values) {
                selected_tracks_.emplace_back(ta, val);
            }

            // Sort by value descending (highest first)
            std::sort(selected_tracks_.begin(), selected_tracks_.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

            // Trim to max selected
            if (selected_tracks_.size() > max_tracks_selected_) {
                selected_tracks_.resize(max_tracks_selected_);
            }
        }

        struct TrackInfo
        {
            uint64_t latest_value{ 0 };
            uint64_t latest_tick_ms{ 0 };
        };

        /**
         * @brief Map of property type value and map of track information by track alias
         *
         * @example tracks_[property_type][track_alias] = TrackInfo
         *
         * TODO: Need to address multi-publisher. Connection id is limiting. Need to support peering as well.
         */
        std::map<PropertyType, std::unordered_map<TrackAlias, TrackInfo>> tracks_;

        /**
         * @brief Selected ordered list of tracks.
         *
         * @details Value is a pair that indicates track alias and the age in milliseconds since last update
         */
        std::vector<TrackSelectedEntry> selected_tracks_;
    };
}
