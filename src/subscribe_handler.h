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
                              ClientManager& server);
        void ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override;
        void StatusChanged(Status status) override;

      private:
        ClientManager& server_;

        uint64_t prev_group_id_{ 0 };
        uint64_t prev_subgroup_id_{ 0 };
    };
} // namespace laps
