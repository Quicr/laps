#pragma once

#include "client_manager.h"

#include <quicr/common.h>
#include <quicr/fetch_track_handler.h>
#include <quicr/object.h>

namespace laps {
    /**
     * @brief Fetch track handler
     * @details Fetch track handler used for the subscribe command line option.
     */
    class FetchTrackHandler : public quicr::FetchTrackHandler
    {
        FetchTrackHandler(const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
                          const quicr::FullTrackName& full_track_name,
                          quicr::messages::ObjectPriority priority,
                          quicr::messages::GroupOrder group_order,
                          quicr::messages::GroupId start_group,
                          quicr::messages::GroupId end_group,
                          quicr::messages::GroupId start_object,
                          quicr::messages::GroupId end_object,
                          ClientManager& server);

      public:
        static std::shared_ptr<FetchTrackHandler> Create(
          const std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler,
          const quicr::FullTrackName& full_track_name,
          quicr::messages::ObjectPriority priority,
          quicr::messages::GroupOrder group_order,
          quicr::messages::GroupId start_group,
          quicr::messages::GroupId end_group,
          quicr::messages::GroupId start_object,
          quicr::messages::GroupId end_object,
          ClientManager& server)
        {
            return std::shared_ptr<FetchTrackHandler>(new FetchTrackHandler(publish_fetch_handler,
                                                                            full_track_name,
                                                                            priority,
                                                                            group_order,
                                                                            start_group,
                                                                            end_group,
                                                                            start_object,
                                                                            end_object,
                                                                            server));
        }

        void ObjectReceived(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override;
        void StatusChanged(Status status) override;

      private:
        std::shared_ptr<quicr::PublishFetchHandler> publish_fetch_handler_;
        ClientManager& server_;
    };
} // namespace laps
