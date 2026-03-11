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
        using TrackEntry = std::vector<std::pair<TrackAlias, uint64_t>>; // Value is the tick value of last update

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
            auto& prop_idx_map = track_entry_index_;
            auto track_it = prop_idx_map.find(track_alias);
            if (track_it != prop_idx_map.end()) {

            }

            auto [prop_it, _] = ordered_tracks_.try_emplace({ prop, value });

            auto entry = std::make_pair(track_alias, tick);
            prop_it->second[track_alias] = tick;

            SPDLOG_INFO("Update Value ta: {} prop: {} value: {} tick: {}", track_alias, prop, value, tick);

            // notify each subscribe namespace (aka publish namespace handler)
            std::vector<quicr::TrackNamespaceHash> rm_handlers;
            for (auto [ns_hash, handler] : ns_handlers_) {
                if (auto h = handler.lock()) {
                    h->UpdateTrackRanking(prop_it->second);
                } else {
                    rm_handlers.emplace_back(ns_hash);
                }
            }

            for (auto hash : rm_handlers) {
                ns_handlers_.erase(hash);
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
        std::map<PropertyType, std::unordered_map<TrackAlias, std::size_t>> track_entry_index_;

        /**
         * @brief Publish namespace handlers that are related to this track ranking
         */
        std::map<quicr::TrackNamespaceHash, std::weak_ptr<PublishNamespaceHandler>> ns_handlers_;
    };
}
