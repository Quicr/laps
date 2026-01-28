#pragma once

#include "client_manager.h"

#include <quicr/common.h>
#include <quicr/object.h>
#include <quicr/subscribe_track_handler.h>

#include <map>

namespace laps {
    /**
     * @brief  Subscribe track handler
     * @details Subscribe track handler used for the subscribe command line option.
     */
    class SubscribeTrackHandler : public quicr::SubscribeTrackHandler
    {
      public:
        SubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                              quicr::messages::ObjectPriority priority,
                              quicr::messages::GroupOrder group_order,
                              ClientManager& server,
                              bool is_publisher_initiated = false);

        void StreamDataRecv(bool is_start,
                            uint64_t stream_id,
                            std::shared_ptr<const std::vector<uint8_t>> data) override;
        void DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data) override;
        void ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override;

        void StatusChanged(Status status) override;

        void SetFromPeer();

        std::optional<uint64_t> GetPendingNewRquestId() { return pending_new_group_request_id_; };

        struct PublisherLastUpdateInfo
        {
            std::optional<std::chrono::time_point<std::chrono::steady_clock>> time;
        } pub_last_update_info_;

      private:
        void ForwardReceivedData(bool is_new_stream,
                                 uint64_t group_id,
                                 uint64_t subgroup_id,
                                 std::shared_ptr<const std::vector<uint8_t>> data);

        ClientManager& server_;

        bool is_datagram_{ false };
        bool is_from_peer_{ false }; // Indicates that the subscribe handler was created by peer manager for recv data
    };
} // namespace laps
