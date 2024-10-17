#pragma once

#include <mutex>
#include <quicr/server.h>
#include <set>

namespace laps {
    class PublishTrackHandler;

    struct State
    {
        std::mutex state_mutex;

        /**
         * Map of subscribes (e.g., track alias) sent to announcements
         *
         * @example
         *      track_alias_set = announce_active[track_namespace_hash][connection_handle]
         */
        std::unordered_map<quicr::TrackNamespaceHash,
                           std::unordered_map<quicr::ConnectionHandle, std::set<quicr::messages::TrackAlias>>>
          announce_active;

        struct SubscribePublishHandlerInfo
        {
            quicr::FullTrackName track_full_name;
            quicr::messages::TrackAlias track_alias;
            quicr::messages::SubscribeId subscribe_id;
            std::shared_ptr<PublishTrackHandler> publish_handler;
        };

        /**
         * Active subscriber publish tracks for a given track, indexed (keyed) by track_alias, connection handle
         *
         * @note This indexing intentionally prohibits per connection having more
         *           than one subscribe to a full track name.
         *
         * @example track_handler = subscribes[track_alias][connection_handle]
         */
        std::unordered_map<quicr::messages::TrackAlias,
                           std::unordered_map<quicr::ConnectionHandle, SubscribePublishHandlerInfo>>
          subscribes;

        /**
         * Subscribe ID to alias mapping
         *      Used to lookup the track alias for a given subscribe ID
         *
         * @example
         *      track_alias = subscribe_alias_sub_id[conn_id][subscribe_id]
         */
        std::unordered_map<quicr::ConnectionHandle,
                           std::unordered_map<quicr::messages::SubscribeId, quicr::messages::TrackAlias>>
          subscribe_alias_sub_id;

        /**
         * Map of subscribes set by namespace and track name hash
         *      Set<subscribe_who> = subscribe_active[track_namespace_hash][track_name_hash]
         */
        struct SubscribeWho
        {
            uint64_t connection_handle;
            uint64_t subscribe_id;
            uint64_t track_alias;

            bool operator<(const SubscribeWho& other) const
            {
                return connection_handle < other.connection_handle && subscribe_id << other.subscribe_id;
            }

            bool operator==(const SubscribeWho& other) const
            {
                return connection_handle == other.connection_handle && subscribe_id == other.subscribe_id;
            }

            bool operator>(const SubscribeWho& other) const
            {
                return connection_handle > other.connection_handle && subscribe_id > other.subscribe_id;
            }
        };

        std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::set<SubscribeWho>>> subscribe_active;

        /**
         * Active publisher/announce subscribes that this relay has made to receive objects from publisher.
         *
         * @example
         *      track_delegate = pub_subscribes[track_alias][conn_id]
         */
        std::unordered_map<quicr::messages::TrackAlias,
                           std::unordered_map<quicr::ConnectionHandle, std::shared_ptr<quicr::SubscribeTrackHandler>>>
          pub_subscribes;
    };
}
