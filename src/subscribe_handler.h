#pragma once

#include "client_manager.h"
#include "publish_namespace_handler.h"

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
        static constexpr uint64_t kRefreshRankingIntervalMs = 120;

        SubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                              quicr::messages::ObjectPriority priority,
                              quicr::messages::GroupOrder group_order,
                              ClientManager& server,
                              std::weak_ptr<quicr::TickService> tick_service,
                              bool is_publisher_initiated = false);

        ~SubscribeTrackHandler();

        void StreamClosed(std::uint64_t stream_id, bool use_reset) override;

        void StreamDataRecv(bool is_start,
                            uint64_t stream_id,
                            std::shared_ptr<const std::vector<uint8_t>> data) override;
        void DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data) override;
        void ObjectReceived(const quicr::ObjectHeaders& object_headers,
                            quicr::BytesSpan data,
                            std::optional<quicr::messages::StreamHeaderProperties> stream_mode = std::nullopt) override;

        void StatusChanged(Status status) override;

        void SetFromPeer();

        std::optional<uint64_t> GetPendingNewRquestId() { return pending_new_group_request_id_; };

        struct PublisherLastUpdateInfo
        {
            std::optional<std::chrono::time_point<std::chrono::steady_clock>> time;
        } pub_last_update_info_;

        /**
         * @brief Add subscriber to publish receive subscribe
         *
         * @param conn_handle           Subscriber connection handle
         * @param request_id            Subscribe request ID which is reused for publish
         * @param priority              Subscriber priority
         * @param delivery_timeout      Subscriber delivery timeout
         * @param start_location        Subscriber requested start location
         */
        void AddSubscriber(quicr::ConnectionHandle conn_handle,
                           quicr::messages::RequestID request_id,
                           uint8_t priority,
                           std::chrono::milliseconds delivery_timeout,
                           quicr::messages::Location start_location);

        /**
         * @brief Remove subscriber from publish fanout
         * @param conn_handle           Subscriber connection handle
         */
        void RemoveSubscriber(quicr::ConnectionHandle conn_handle);

        /**
         * @brief Add subscribe namespace publish namespace handler
         *
         * @param handler               Publish namespace handler to use to send matching tracks
         */
        void AddSubscribeNamespace(std::shared_ptr<PublishNamespaceHandler> handler);

        /**
         * @brief Remove subscribe namespace publish namespace handler
         *
         * @param handler               Publish namespace handler used to send matching tracks
         */
        void RemoveSubscribeNamespace(std::shared_ptr<PublishNamespaceHandler> handler);

        void SetTrackRanking(std::weak_ptr<TrackRanking> track_ranking) { track_ranking_ = std::move(track_ranking); }

        bool HasSubscribers() const { return !subscribers_.empty() || !sub_namespaces_.empty(); }

      private:
        void ForwardReceivedData(bool is_new_stream,
                                 uint64_t group_id,
                                 uint64_t subgroup_id,
                                 std::shared_ptr<const std::vector<uint8_t>> data);

        void UpdateTrackedProperties(std::optional<quicr::Extensions> extensions,
                                     std::optional<quicr::Extensions> immutable_extensions);

        ClientManager& server_;
        std::weak_ptr<quicr::TickService> tick_service_;

        bool is_datagram_{ false };
        bool is_from_peer_{ false }; // Indicates that the subscribe handler was created by peer manager for recv data

        /**
         * @brief Map of subscribers that have subscribed to this content
         *
         * @details Fanout list of subscribe publish handlers.  On subscribe, this list is updated.
         *
         * @
         */
        std::map<quicr::ConnectionHandle, std::shared_ptr<PublishTrackHandler>> subscribers_;

        /**
         * @brief Map of publish namespace handlers by subscribe namespace full track name hash and connection handle
         */
        std::map<quicr::TrackFullNameHash, std::map<quicr::ConnectionHandle, std::shared_ptr<PublishNamespaceHandler>>>
          sub_namespaces_;

        /**
         * @brief property values
         * @details
         */
        std::map<uint64_t, PublishNamespaceHandler::TrackPropertyValue> tracked_properties_value_;

        std::weak_ptr<TrackRanking> track_ranking_;
    };
} // namespace laps
