#pragma once

#include "quicr/publish_namespace_handler.h"
#include "quicr/track_name.h"

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
    };
} // namespace laps
