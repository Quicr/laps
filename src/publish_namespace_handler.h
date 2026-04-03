#pragma once

#include "quicr/publish_namespace_handler.h"
#include "quicr/track_name.h"

#include <unordered_map>

namespace laps {
    class PublishTrackHandler;

    /**
     * @brief  Publish namespace handler
     */
    class PublishNamespaceHandler : public quicr::PublishNamespaceHandler
    {
      public:
        /**
         * @brief Tracked property value struct
         * @details Structure of variables for tracked property values
         */
        struct TrackPropertyValue
        {
            uint64_t latest_value;                       // Latest value sampled
            quicr::TickService::TickType latest_tick_ms; // Latest tick value when last sampled
        };

        PublishNamespaceHandler(const quicr::TrackNamespace& prefix, std::weak_ptr<quicr::TickService> tick_service);

        void EndSubgroup(uint64_t group_id, uint64_t subgroup_id, bool completed);

        quicr::PublishTrackHandler::PublishObjectStatus PublishObject(quicr::TrackFullNameHash track_full_name_hash,
                                                                      const quicr::ObjectHeaders& object_headers,
                                                                      quicr::BytesSpan data) override;

        quicr::PublishTrackHandler::PublishObjectStatus ForwardPublishedData(
          quicr::TrackFullNameHash track_full_name_hash,
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data) override;

        void PublishTrack(std::shared_ptr<quicr::PublishTrackHandler> handler) override;

        static auto Create(const quicr::TrackNamespace& prefix, std::weak_ptr<quicr::TickService> tick_service)
        {
            return std::shared_ptr<PublishNamespaceHandler>(new PublishNamespaceHandler(prefix, tick_service));
        }

        /**
         * @brief Updates the track ranking
         * @details TrackRanking instance calls this method for each namespace to update the top-n/ranked tracks
         *
         * @param ordered_tracks            Span of track property values with sequence number, latest tick, and
         * connection IDs
         */
        virtual void UpdateTrackRanking(
          std::span<const std::tuple<quicr::messages::TrackAlias, uint64_t, uint64_t, uint64_t>> ordered_tracks);

        /*
         * Getter/Setters
         */
        constexpr void SetMaxSelected(const uint64_t max) { max_tracks_selected_ = max; }
        constexpr uint64_t GetMaxSelected() { return max_tracks_selected_; }

        constexpr void SetInactiveAge(const uint64_t age_ms) { inactive_age_ms_ = age_ms; }
        constexpr uint64_t GetInactiveAge() { return inactive_age_ms_; }

        constexpr void SetPropertyType(const uint64_t prop) { property_type_ = prop; }
        constexpr uint64_t GetPropertyType() { return property_type_.value_or(0); }

      private:
        uint64_t max_tracks_selected_{ 1 };       // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 10000 };       // Age in ms of a track that is considered stale/inactive
        std::optional<uint64_t> property_type_;   // Property type to rank by
        uint64_t delay_publish_done_ms_{ 20000 }; // Delay sending publish done to allow publish to come back

        std::weak_ptr<quicr::TickService> tick_service_;

        struct ActiveTrack
        {
            uint64_t last_updated_tick;
            std::weak_ptr<quicr::PublishTrackHandler> handler;
        };

        std::map<quicr::messages::TrackAlias, ActiveTrack> published_tracks_;
    };
} // namespace laps
