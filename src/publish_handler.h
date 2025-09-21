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

        /**
         * @brief Check if the first object has been sent or not
         *
         * @return True if one objet has been sent, False if no objects yet
         */
        constexpr bool SentFirstObject() const noexcept { return latest_object_id_.has_value(); }

      private:
        ClientManager& server_;
    };

} // namespace laps
