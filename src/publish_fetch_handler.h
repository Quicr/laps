#pragma once

#include "client_manager.h"

#include <quicr/publish_fetch_handler.h>

namespace laps {

    /**
     * @brief Publish-side fetch handler with optional MoQ binding (no libquicr transport).
     * @details When `BindForMoxygenFetch` has been called, publish/forward attempts use
     *          `ClientManager::TryMoxygenFetch*`; otherwise the libquicr transport path is used.
     */
    class PublishFetchHandler final : public quicr::PublishFetchHandler
    {
        PublishFetchHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::ObjectPriority priority,
                            uint64_t subscribe_id,
                            quicr::messages::GroupOrder group_order,
                            uint32_t ttl,
                            ClientManager& client_manager);

      public:
        static std::shared_ptr<PublishFetchHandler> Create(const quicr::FullTrackName& full_track_name,
                                                             quicr::messages::ObjectPriority priority,
                                                             uint64_t subscribe_id,
                                                             quicr::messages::GroupOrder group_order,
                                                             uint32_t ttl,
                                                             ClientManager& client_manager)
        {
            return std::shared_ptr<PublishFetchHandler>(new PublishFetchHandler(
              full_track_name, priority, subscribe_id, group_order, ttl, client_manager));
        }

        /** @brief Sets connection and synthetic data context for MoQ port fetch response (no ITransport). */
        void BindForMoxygenFetch(quicr::ConnectionHandle connection_handle, uint64_t publish_data_ctx_id);

        bool IsMoqFetchPortBound() const noexcept { return moq_fetch_bound_; }

        quicr::PublishTrackHandler::PublishObjectStatus PublishObject(const quicr::ObjectHeaders& object_headers,
                                                                      quicr::BytesSpan data) override;

        quicr::PublishTrackHandler::PublishObjectStatus ForwardPublishedData(
          bool is_new_stream,
          uint64_t group_id,
          uint64_t subgroup_id,
          std::shared_ptr<const std::vector<uint8_t>> data);

      private:
        ClientManager& client_manager_;
        bool moq_fetch_bound_{ false };
    };

} // namespace laps
