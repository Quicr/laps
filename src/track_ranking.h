#pragma once

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

                    // compute
                } else if (t_it->second.latest_value > value) {
                    SPDLOG_INFO("Decrease in value TA: {} type: {} from: {} to: {} tick: {}",
                                track_alias,
                                prop,
                                t_it->second.latest_value,
                                value,
                                tick);

                    // compute
                }

                // Update
                t_it->second.latest_value = value;
                t_it->second.latest_tick_ms = tick;
            }
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
         * @details Returns a list of track aliases and the age in milliseconds since the last
         *      update. Limit is used to limit the result set.
         *
         * @param limit           Limit the return set of selected, ordered by best first
         *
         * @returns Returns a span of pairs <TrackAlias, ms_since_last_update>
         */
        constexpr std::span<const TrackSelectedEntry> GetSelected(uint64_t limit) { return {}; }

      private:
        uint64_t max_tracks_selected_{ 32 }; // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 5000 };   // Age in ms of a track that is considered stale/inactive

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
