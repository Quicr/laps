#pragma once

#include "client_manager.h"
#include "state.h"
#include <quicr/common.h>
#include <quicr/subscribe_track_handler.h>

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

        // note: pipelining starts after the first object
        bool SentFirstObject(uint32_t group_id, uint32_t subgroup_id);

      private:
        ClientManager& server_;

      public:
        /*
         * Filter related variables
         */
        quicr::messages::Location start_location_{ 0, 0 };
    };

} // namespace laps
