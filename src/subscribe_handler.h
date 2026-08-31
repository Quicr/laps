#pragma once

#include "client_manager.h"
#include "publish_namespace_handler.h"

#include <quicr/handlers/subscribe_track_handler.h>
#include <quicr/messages/object.h>

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
                              std::uint8_t priority,
                              std::optional<quicr::messages::GroupOrder> group_order,
                              ClientManager& server,
                              std::weak_ptr<timeq::tick_service> tick_service,
                              bool is_publisher_initiated = false);

        ~SubscribeTrackHandler();

        void StreamClosed(std::uint64_t stream_id, bool use_reset) override;

        void StreamDataRecv(uint64_t stream_id, quicr::InitialStreamData&& initial_buffer) override;
        void StreamDataRecv(uint64_t stream_id, std::shared_ptr<const std::vector<uint8_t>> data) override;
        void DgramDataRecv(std::shared_ptr<const std::vector<uint8_t>> data) override;
        void ObjectReceived(const quicr::ObjectHeaders& object_headers,
                            quicr::BytesSpan data,
                            std::optional<quicr::messages::StreamHeaderProperties> stream_mode = std::nullopt) override;
        void MetricsSampled(const quicr::SubscribeTrackMetrics& metrics) override;

        void StatusChanged(Status status) override;

        void SetFromPeer();

        /**
         * @brief Record that forwarding is paused without sending a subscribe update
         *
         * @details A publisher-initiated subscribe is not bound to its connection until the relay accepts the
         *      publish, so Pause() has no session to send the update on. The relay instead answers the publish
         *      with forwarding off, and uses this so that a later Resume() sends the update.
         */
        void MarkPaused() { SetStatus(Status::kPaused); }

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
        void AddSubscriber(std::uint64_t conn_handle,
                           std::uint64_t request_id,
                           uint8_t priority,
                           std::chrono::milliseconds delivery_timeout,
                           quicr::messages::Location start_location);

        /**
         * @brief Remove subscriber from publish fanout
         * @param conn_handle           Subscriber connection handle
         */
        void RemoveSubscriber(std::uint64_t conn_handle);

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

        /**
         * @brief Number of subscribers receiving fanout of this track via SUBSCRIBE
         *
         * @note Subscribers matched through a subscribe namespace are not included.
         */
        std::size_t SubscriberCount() const { return subscribers_.size(); }

      private:
        void TryProcessStreamData(uint64_t stream_id, StreamContext& stream);

        void ForwardReceivedData(bool is_new_stream,
                                 uint64_t group_id,
                                 uint64_t subgroup_id,
                                 std::shared_ptr<const std::vector<uint8_t>> data,
                                 bool forward_to_peers = true);

        void UpdateTrackedProperties(std::optional<quicr::Extensions> extensions,
                                     std::optional<quicr::Extensions> immutable_extensions);

        ClientManager& server_;
        std::weak_ptr<timeq::tick_service> tick_service_;

        bool is_datagram_{ false };
        bool is_from_peer_{ false }; // Indicates that the subscribe handler was created by peer manager for recv data

        // Original receive buffers retained until the subgroup header is complete so they can be forwarded without
        // copying when the track alias is unchanged.
        std::map<std::uint64_t, std::vector<std::shared_ptr<const std::vector<uint8_t>>>> pending_source_buffers_;

        /**
         * @brief Map of subscribers that have subscribed to this content
         *
         * @details Fanout list of subscribe publish handlers.  On subscribe, this list is updated.
         *
         * @
         */
        std::map<std::uint64_t, std::shared_ptr<PublishTrackHandler>> subscribers_;

        /**
         * @brief Map of publish namespace handlers by subscribe namespace full track name hash and connection handle
         */
        std::map<std::uint64_t, std::map<std::uint64_t, std::shared_ptr<PublishNamespaceHandler>>> sub_namespaces_;

        /**
         * @brief property values
         * @details
         */
        std::map<uint64_t, PublishNamespaceHandler::TrackPropertyValue> tracked_properties_value_;

        std::weak_ptr<TrackRanking> track_ranking_;
    };
} // namespace laps
