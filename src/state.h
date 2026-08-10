#pragma once

#include "publish_namespace_handler.h"

#include <mutex>
#include <quicr/session.h>
#include <set>

namespace laps {
    class SubscribeTrackHandler;
    class PublishTrackHandler;

    struct State
    {
        std::mutex state_mutex;

        /**
         * Request Transaction struct to state track the active request
         */
        struct RequestTransaction
        {
            enum class Type : uint8_t
            {
                kSubscribeNamespace,
                kPublishNamespace,
            };

            enum class State : uint8_t
            {
                kOk,
                kPendingOk,
                kError,
            };

            Type type;             ///< Type of request
            State state;           ///< State of the request
            std::any related_data; ///< Related data based on the type
        };

        /**
         * Active requests by connection handle and request ID
         */
        std::map<std::pair<std::uint64_t, std::uint64_t>, RequestTransaction> requests;

        /**
         * Map of subscribes (e.g., track alias) matched to a publish namespace
         *
         * @example
         *      track_alias_set = namespace_active[track_namespace_hash, connection_handle]
         */
        std::map<std::pair<quicr::TrackNamespace, std::uint64_t>, std::set<std::uint64_t>> pub_namespace_active;

        /**
         * Active publisher/announce subscribes that this relay has made to receive objects from publisher.
         *
         * @example
         *      track_delegate = pub_subscribes[track_alias, connection handle]
         */
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<SubscribeTrackHandler>> pub_subscribes;

        /**
         * Active publisher initiated subscribes by request Id and connection handle
         */
        std::map<std::pair<uint64_t, std::uint64_t>, std::shared_ptr<SubscribeTrackHandler>> pub_subscribes_by_req_id;

        /**
         * @brief Subscribe Namespace by connection to publish namespace handlers
         * @details Subscribe namespaces by connection are added to this map. Each will have an associated
         *      publish namespace handler. The publish namespace handler is used to establish publish tracks
         *      to the subscriber of the namespace
         */
        std::map<quicr::TrackNamespace, std::map<std::uint64_t, std::shared_ptr<PublishNamespaceHandler>>>
          subscribes_namespaces;

        struct SubscribePublishHandlerInfo
        {
            quicr::FullTrackName track_full_name;
            std::uint64_t track_alias{ 0 };
            std::uint64_t request_id{ 0 };
            uint8_t priority{ 0 };
            uint32_t object_ttl{ 0 };
            std::optional<quicr::messages::GroupOrder> group_order;
            quicr::messages::Location start_location;
        };

        /**
         * @brief Get the count of publish tracks
         *
         * @param connection_handle     Connection handle/id
         *
         * @return Number of publishing tracks per the connection
         */
        std::size_t PublishTrackCount(std::uint64_t connection_handle)
        {
            std::lock_guard _(state_mutex);

            const auto& metrics = conn_state_metrics.find(connection_handle);
            if (metrics != conn_state_metrics.end()) {
                return metrics->second.published_tracks;
            }

            return 0;
        }

        /**
         * Active subscriber publish tracks for a given track, indexed (keyed) by track_alias, connection handle
         *
         * @note This indexing intentionally prohibits per connection having more
         *           than one subscribe to a full track name.
         *
         * @example track_handler = subscribes[track_alias, connection_handle]
         */
        std::map<std::pair<std::uint64_t, std::uint64_t>, SubscribePublishHandlerInfo> subscribes;

        struct StateMetrics
        {
            std::uint64_t subscribed_tracks; ///< gauge; Set to the active number of subscribes
            std::uint64_t published_tracks;  // gauge; Set to the active number of publishes
        };

        // Key is connection ID/handle
        std::map<std::uint64_t, StateMetrics> conn_state_metrics;

        /**
         * @brief Get the count of subscribe tracks
         *
         * @param connection_handle     Connection handle/id
         *
         * @return Number of subscribes received from the connection, including the tracks matched by each of
         *      the connection's namespace subscribes
         */
        std::size_t SubscribeTrackCount(std::uint64_t connection_handle)
        {
            std::lock_guard _(state_mutex);

            const auto& metrics = conn_state_metrics.find(connection_handle);
            if (metrics != conn_state_metrics.end()) {
                return metrics->second.subscribed_tracks;
            }

            return 0;
        }

        /**
         * Request ID to alias mapping
         *      Used to lookup the track alias for a given request ID
         *
         * @example
         *      track_alias = subscribe_alias_req_id[connection handle, request_id]
         */
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> subscribe_alias_req_id;

        /**
         * Map of subscribes set by namespace and track name hash
         *      Set<subscribe_who> = subscribe_active[track_namespace_hash, track_name_hash]
         */
        struct SubscribeInfo
        {
            uint64_t connection_handle;
            uint64_t request_id;
            uint64_t track_alias;
            uint8_t priority;
            std::chrono::milliseconds delivery_timeout;
            quicr::messages::Location start_location;

            bool operator<(const SubscribeInfo& other) const
            {
                return connection_handle < other.connection_handle ||
                       (connection_handle == other.connection_handle && request_id < other.request_id);
            }

            bool operator==(const SubscribeInfo& other) const
            {
                return connection_handle == other.connection_handle && request_id == other.request_id;
            }

            bool operator>(const SubscribeInfo& other) const
            {
                return connection_handle > other.connection_handle ||
                       (connection_handle == other.connection_handle && request_id > other.request_id);
            }
        };

        std::map<std::pair<quicr::TrackNamespace, std::uint64_t>, std::set<SubscribeInfo>> subscribe_active_;
    };
}
