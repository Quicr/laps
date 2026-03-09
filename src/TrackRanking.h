#pragma once

#include "spdlog/fmt/bundled/format-inl.h"

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
        using TrackAlias = uint64_t;
        using TrackSelectedEntry = std::pair<TrackAlias, uint64_t>;

        void UpdateValue(TrackAlias track_alias, uint64_t prop, uint64_t value)
        {
            auto [it, is_new] = tracks_.try_emplace(prop);
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
        std::map<uint64_t, std::unordered_map<TrackAlias, TrackInfo>> tracks_;

        /**
         * @brief Selected ordered list of tracks.
         *
         * @details Value is a pair that indicates track alias and the age in milliseconds since last update
         */
        std::vector<TrackSelectedEntry> selected_tracks_;
    };
}
