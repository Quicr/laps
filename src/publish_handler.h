#pragma once

#include "client_manager.h"

namespace laps {
    /**
     * @brief Publish track handler
     * @details Publish track handler used for the publish command line option
     */
    class PublishTrackHandler : public quicr::PublishTrackHandler
    {
      public:
        PublishTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::TrackMode track_mode,
                            uint8_t default_priority,
                            uint32_t default_ttl,
                            quicr::messages::Location start_location,
                            ClientManager& server);

        void StatusChanged(Status status) override;
        void MetricsSampled(const quicr::PublishTrackMetrics& metrics) override;

        PublishObjectStatus PublishObject(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override;

        /** @brief Hides non-virtual base; enables moxygen relay byte forwarding when applicable. */
        PublishObjectStatus ForwardPublishedData(bool is_new_stream,
                                                 uint64_t group_id,
                                                 uint64_t subgroup_id,
                                                 std::shared_ptr<const std::vector<uint8_t>> data);

        // note: pipelining starts after the first object
        bool SentFirstObject(uint32_t group_id, uint32_t subgroup_id);

        void SetBaseTrackAlias(uint64_t track_alias) { track_alias_ = track_alias; }

        void AbruptCloseAllSubgroups();

        static std::shared_ptr<PublishTrackHandler> Create(const quicr::FullTrackName& full_track_name,
                                                           quicr::TrackMode track_mode,
                                                           uint8_t default_priority,
                                                           uint32_t default_ttl,
                                                           quicr::messages::Location start_location,
                                                           ClientManager& server)
        {
            return std::make_shared<PublishTrackHandler>(
              full_track_name, track_mode, default_priority, default_ttl, start_location, server);
        }

      private:
        ClientManager& server_;
        quicr::TrackMode publish_track_mode_{ quicr::TrackMode::kStream };

      public:
        /*
         * Filter related variables
         */
        quicr::messages::Location start_location_{ 0, 0 };
    };

} // namespace laps
