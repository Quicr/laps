#pragma once

#include "server.h"

#include "state.h"
#include <quicr/common.h>
#include <quicr/object.h>
#include <quicr/subscribe_track_handler.h>

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
                              LapsServer& server);
        void ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override;
        void StatusChanged(Status status) override;

      private:
        LapsServer& server_;
    };
} // namespace laps
