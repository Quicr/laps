// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "common.h"
#include "peer_session.h"
#include "subscribe_info.h"

#include <map>
#include <quicr/detail/messages.h>

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

        struct NodeItem
        {
          std::weak_ptr<PeerSession> peer_session;
          NodeInfo node_info;
        };

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
         * @brief Removes all nodes associated to the peer session id
         */
        void RemoveNodes(PeerSessionId peer_session_id);

        /**
         * @brief Gets the best peer session for given node id
         */
        PeerSession& GetBestPeerSession(NodeIdValueType node_id);

        /**
         * @brief Selects and updates the best peer session to use for given node id
         * @details Implements the selection algorithm to find the best peering session
         *   to reach the node.
         *
         * @return True if node is better and updated, False if not
         */
        bool SelectBestNode(NodeIdValueType node_id);

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
        //std::map<quicr::messages::TrackAlias, std::set<decltype(nodes_best_)::mapped_type&>> subscribes;
        std::map<quicr::messages::TrackAlias, std::set<NodeIdValueType>> subscribes;

          /**
           * @brief Nodes by peer session id
           * @details This map is updated whenever nodes_ is updated. It's used when cleaning up the other node tables
           *   on peer disconnect/cleanup.
           */
          std::map<PeerSessionId, std::set<NodeIdValueType>> nodes_by_peer_session_;
        std::mutex mutex_;

      private:
    };

}