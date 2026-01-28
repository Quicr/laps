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
                            ClientManager& server);

        void StatusChanged(Status status) override;
        void MetricsSampled(const quicr::PublishTrackMetrics& metrics) override;

        bool pipeline_{ false }; // True indicates using pipeline forwarding, false is object forwarding

      private:
        ClientManager& server_;
    };

} // namespace laps
