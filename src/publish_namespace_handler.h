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

        PublishNamespaceHandler(const quicr::TrackNamespace& prefix);

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

        static auto Create(const quicr::TrackNamespace& prefix)
        {
            return std::shared_ptr<PublishNamespaceHandler>(new PublishNamespaceHandler(prefix));
        }

        /**
         * @brief Updates the track ranking
         * @details TrackRanking instance calls this method for each namespace to update the top-n/ranked tracks
         *
         * @param ordered_tracks            Span of track proprtery values
         */
        virtual void UpdateTrackRanking(
          const std::unordered_map<quicr::messages::TrackAlias, uint64_t>& ordered_tracks);

        /*
         * Getter/Setters
         */
        constexpr void SetMaxSelected(const uint64_t max) { max_tracks_selected_ = max; }
        constexpr uint64_t GetMaxSelected() { return max_tracks_selected_; }

        constexpr void SetInactiveAge(const uint64_t age_ms) { inactive_age_ms_ = age_ms; }
        constexpr uint64_t GetInactiveAge() { return inactive_age_ms_; }

      private:
        uint64_t max_tracks_selected_{ 3 }; // Max tracks to select as candidate top-n
        uint64_t inactive_age_ms_{ 3000 };  // Age in ms of a track that is considered stale/inactive

        std::map<quicr::TrackNamespaceHash, std::weak_ptr<quicr::PublishTrackHandler>> active_tracks_;
    };
} // namespace laps
