#pragma once

#include "client_manager.h"

#include <quicr/common.h>
#include <quicr/handlers/fetch_track_handler.h>

namespace laps {
    /**
     * @brief Fetch track handler
     * @details Fetch track handler used for the subscribe command line option.
     */
    class FetchTrackHandler : public quicr::FetchTrackHandler
    {
        FetchTrackHandler(const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
                          const quicr::FullTrackName& full_track_name,
                          std::uint64_t priority,
                          std::optional<quicr::messages::GroupOrder> group_order,
                          const quicr::messages::Location& start_location,
                          const quicr::messages::FetchEndLocation& end_location);

      public:
        static std::shared_ptr<FetchTrackHandler> Create(
          const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
          const quicr::FullTrackName& full_track_name,
          std::uint64_t priority,
          std::optional<quicr::messages::GroupOrder> group_order,
          const quicr::messages::Location& start_location,
          const quicr::messages::FetchEndLocation& end_location)
        {
            return std::shared_ptr<FetchTrackHandler>(new FetchTrackHandler(
              publish_fetch_handler, full_track_name, priority, group_order, start_location, end_location));
        }

        void StatusChanged(Status status) override;
        void StreamDataRecv(uint64_t stream_id, quicr::InitialStreamData&& initial_buffer) override;
        void StreamDataRecv(uint64_t stream_id, std::shared_ptr<const std::vector<uint8_t>> data) override;

      private:
        void TryForwardInitialStreamData(uint64_t stream_id, StreamContext& stream);

        bool initial_stream_data_forwarded_{ false };
        std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler_;
    };
} // namespace laps
