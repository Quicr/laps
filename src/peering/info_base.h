// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "common.h"
#include "peer_session.h"
#include "peering/messages/subscribe_info.h"

#include <map>
#include <quicr/detail/messages.h>
#include <set>

namespace laps::peering {

    /**
     * @brief Forwarding information base
     * @details Computed subscribe and announcements are added and maintained
     */
    class InfoBase
    {
      public:
        InfoBase() = default;
        virtual ~InfoBase() = default;

        /**
         * @brief Add or update node in the info base
         * @details This will add or update a node in the info base. Upon update, other tables
         *   will be updated to compute the best node based on change.
         *
         * @returns True if the node is updated as best
         */
        bool AddNode(std::shared_ptr<PeerSession> peer_session, const NodeInfo& node_info);

        /**
         * @brief Remove node from the info base
         */
        void RemoveNode(PeerSessionId peer_session_id, NodeIdValueType node_id);

        /**
         * @brief Purge peer session information
         */
        void PurgePeerSessionInfo(PeerSessionId peer_session_id);

        /**
         * @brief Add or update subscribe in the info base
         * @details This will add or update a subscribe in the info base.
         *
         * @returns True if subscribe is new
         */
        bool AddSubscribe(const SubscribeInfo& subscribe_info);

        /**
         * @brief Remove subscribe from the info base
         *
         * @returns True if the subscribe was removed
         */
        bool RemoveSubscribe(const SubscribeInfo& subscribe_info);

        /**
         * @brief Has Subscribers
         * @details Method to check if there are active peer subscribers
         * @returns True to indicate there are peer subscribers, false to indicate no peer subscribers
         */
        bool HasSubscribers(const SubscribeInfo& subscribe_info);

        /**
         * @brief Gets subscribe information
         * @details Get the subscribe information for track fullname hash and source node id
         *
         * @param track_fullname_hash       Track fullname has for the client manager subscription
         * @param src_node_id               Source node ID for the subscribe info
         *
         * @return SubscribeInfo detailing for the matching subscribe, nullopt if not found
         */
        std::optional<SubscribeInfo> GetSubscribe(uint64_t track_fullname_hash, NodeIdValueType src_node_id);

        /**
         * @brief Add or update announce in the info base
         * @details This will add or update a announce in the info base.
         *
         * @returns True if subscribe is new
         */
        bool AddAnnounce(const AnnounceInfo& announce_info);

        /**
         * @brief Remove announce from the info base
         *
         * @returns True if the announce was removed
         */
        bool RemoveAnnounce(const AnnounceInfo& announce_info);

        /**
         * @brief Get matching (prefix matched) announce source node Ids
         *
         * @details Will return the node ids of the o-relay(s) that have publishers that prefix match
         *      the namespace. If name has value, then the name will be used to return a full match on
         *      namespace and name. If exact is true, then the only node ids return will be exact match
         *      on namespace and name. If exact is false, then node ids will be returned matching
         *      the name_space as a prefix lookup.
         *
         * @param name_space        Namespace or prefix namespace used to prefix match announces to find ids
         * @param name              Zero len if no name. If present, the name_space and name will be used
         * @param exact             True to indicate full namespace and name match. Name MUST be defined
         *                          False indicates return all matching name_space when name is not an exact match
         *
         * @returns a set of announce source node Ids
         */
        std::set<NodeIdValueType> GetAnnounceIds(quicr::messages::TrackNamespace name_space,
                                                 quicr::messages::TrackName name,
                                                 bool exact);

        /**
         * @brief Gets the best peer session for given node id
         */
        std::weak_ptr<PeerSession> GetBestPeerSession(NodeIdValueType node_id);

        /**
         * @brief Selects and updates the best peer session to use for given node id
         * @details Implements the selection algorithm to find the best peering session
         *   to reach the node.
         *
         * @return True if node is better and updated, False if not
         */
        bool SelectBestNode(NodeIdValueType node_id);

        struct NodeItem
        {
            std::weak_ptr<PeerSession> peer_session;
            NodeInfo node_info;
        };

        /**
         * @brief Table of nodes (all received node info), indexed by node id and peer session id
         * @details Nodes are inserted into the node map/table by peer session id. Duplicates by peer
         *    session id are replaced with the most current one.  Upon inserting into this map, the nodes_best_
         *    and nodes_by_peer_session_ maps will be updated.
         */
        std::map<std::pair<NodeIdValueType, PeerSessionId>, NodeItem> nodes_;

        /**
         * @brief Best selected peer session for node id
         * @details Indexed by node id. This is updated whenever the nodes_ table is
         *    updated. This map establishes a data-plane to reach a given node via the best
         *    peering session.
         */
        std::unordered_map<NodeIdValueType, std::weak_ptr<PeerSession>> nodes_best_;

        std::map<quicr::TrackFullNameHash, std::map<NodeIdValueType, SubscribeInfo>> subscribes_;

        struct FibEntry
        {
            uint64_t update_ref{ 0 };      ///< Random reference number to detect if entry was updated or not
            uint64_t stream_id{ 0 };       ///< Current stream ID
            SubscribeNodeSetId out_sns_id; ///< Egress SNS ID
            decltype(nodes_best_)::mapped_type peer_session;
        };

        /**
         * @brief Client forwarding information base (table)
         * @details Client published objects uses this map to forward the objects to
         *   peers based on peer subscribes and best peer to reach the subscribing node. This map
         *   is updated by peering manager on peer received subscribes when relay has local
         *   announcement matching the subscribe
         *
         *   Key is the track full name hash and the egress peer session id
         */
        std::map<std::pair<quicr::TrackFullNameHash, PeerSessionId>, FibEntry> client_fib_;

        /**
         * @brief Peer forwarding information base (table)
         * @details Peer receives subscribe node sets (SNS) via control channel. The SNS has a session scope
         *   ID that is set by the sender of data objects. Upon receiving the data object the
         *   SNS ID is looked up to forward data to other peers using an egress SNS ID for that peer session.
         *   A session Id=0 and SNS ID=0 indicates that this relay has a client that is interested and that
         *   the object should be sent to the client manager to fan out the data to subscribers.
         *
         *   Key is the ingress peer session id and sns id. Value is the egress peer session id and fib entry
         */
        std::map<std::pair<PeerSessionId, SubscribeNodeSetId>, std::map<PeerSessionId, FibEntry>> peer_fib_;

        /**
         * @brief State map of announces received
         *
         * @details State map tracks both PUBLISH and PUBLISH_NAMESPACE. Name does not have to be defined.
         */
        std::map<quicr::TrackFullNameHash, std::map<NodeIdValueType, AnnounceInfo>> announces_;

        /**
         * @brief State map of prefix matchable tuple hashes to full announce namespace/name hash
         *
         * @details Announces are indexed by the full hash of all tuples and name. In order to
         *      do prefix matching, each tuple needs to be evaulated from first to last.  Using a hash
         *      of the tuple results in a complex nested map with varialble number of tuple (sub maps).  This
         *      degrades performance and memory.  This second map is used for fast lookup supporting prefix
         *      matching by hashing each tuple using the combined hash of the previous tuples.  This produces
         *      a flat table of namespace tuple hashes that can be looked up using O(1) to find the prefix hash
         *      that matches the lookup prefix hash.  The vaule is a set of full hash values to be used
         *      to find in the announces_ state map.
         */
        std::map<quicr::TrackNamespaceHash, std::set<quicr::TrackNamespaceHash>> prefix_lookup_announces_;

        /**
         * @brief Nodes by peer session id
         * @details This map is updated whenever nodes_ is updated. It's used when cleaning up the other node tables
         *   on peer disconnect/cleanup.
         */
        std::map<PeerSessionId, std::set<NodeIdValueType>> nodes_by_peer_session_;
        std::mutex mutex_;

      private:
        static std::vector<std::size_t> PrefixHashNamespaceTuples(const quicr::TrackNamespace& name_space);
    };

}