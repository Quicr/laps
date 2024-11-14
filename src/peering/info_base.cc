// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "info_base.h"

namespace laps::peering {
    bool InfoBase::AddNode(std::shared_ptr<PeerSession> peer_session, const NodeInfo& node_info)
    {
        std::lock_guard _(mutex_);

        auto it = nodes_.find({ node_info.id, peer_session->GetSessionId() });
        if (it == nodes_.end()) {
            // New node entry
            nodes_.try_emplace({ node_info.id, peer_session->GetSessionId() }, NodeItem{ peer_session, node_info });
        } else {
            // Existing update
            it->second = { peer_session, node_info };
        }

        nodes_by_peer_session_[peer_session->GetSessionId()].emplace(node_info.id);

        return SelectBestNode(node_info.id);
    }

    void InfoBase::RemoveNode(PeerSessionId peer_session_id, NodeIdValueType node_id)
    {
        std::lock_guard _(mutex_);

        auto ids_it = nodes_by_peer_session_.find(peer_session_id);
        if (ids_it != nodes_by_peer_session_.end()) {
            ids_it->second.erase(node_id);

            if (ids_it->second.empty()) {
                nodes_by_peer_session_.erase(ids_it);
            }
        }

        nodes_.erase({ node_id, peer_session_id });

        if (nodes_best_.erase(node_id) > 0) {
            SelectBestNode(node_id);
        }
    }

    void InfoBase::PurgePeerSessionInfo(PeerSessionId peer_session_id)
    {
        std::vector<NodeIdValueType> node_ids;
        node_ids.reserve(10);

        std::lock_guard _(mutex_);

        auto ids_it = nodes_by_peer_session_.find(peer_session_id);
        if (ids_it != nodes_by_peer_session_.end()) {
            for (const auto& node_id : ids_it->second) {
                nodes_.erase({ node_id, peer_session_id });

                if (nodes_best_.erase(node_id) > 0) {
                    SelectBestNode(node_id);
                }
            }

            nodes_by_peer_session_.erase(ids_it);
        }

        // TODO: Remove subscribes
        // TODO: Remove announces
    }

    bool InfoBase::AddSubscribe(const SubscribeInfo& subscribe_info)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_.find(subscribe_info.id);
        if (it == subscribes_.end()) {
            subscribes_[subscribe_info.id].emplace(subscribe_info.source_node_id);
            return true;
        }

        const auto [__, is_new] = it->second.emplace(subscribe_info.source_node_id);
        return is_new;
    }

    bool InfoBase::RemoveSubscribe(const SubscribeInfo& subscribe_info)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_.find(subscribe_info.id);

        if (it == subscribes_.end()) {
            if (it->second.erase(subscribe_info.source_node_id)) {
                return true;
            }
        }

        return false;
    }

    bool InfoBase::AddAnnounce(const AnnounceInfo& announce_info)
    {
        std::lock_guard _(mutex_);

        auto it = announces_.find(announce_info.full_name.full_name_hash);
        if (it == announces_.end()) {
            announces_[announce_info.full_name.full_name_hash].emplace(announce_info.source_node_id);
            return true;
        }

        const auto [__, is_new] = it->second.emplace(announce_info.source_node_id);
        return is_new;
    }

    bool InfoBase::RemoveAnnounce(const AnnounceInfo& announce_info)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_.find(announce_info.full_name.full_name_hash);

        if (it == subscribes_.end()) {
            if (it->second.erase(announce_info.source_node_id)) {
                return true;
            }
        }

        return false;
    }

    PeerSession& InfoBase::GetBestPeerSession(NodeIdValueType node_id)
    {
        std::lock_guard _(mutex_);

        auto it = nodes_best_.find(node_id);
        if (it != nodes_best_.end()) {
            return *it->second.lock().get();
        }

        throw std::runtime_error("Peer session not found");
    }

    bool InfoBase::SelectBestNode(NodeIdValueType node_id)
    {
        bool is_updated{ false };

        std::weak_ptr<PeerSession> best_peer_session;

        NodeInfo best_node_info;
        auto best_it = nodes_best_.find(node_id);
        if (best_it != nodes_best_.end()) {
            best_peer_session = best_it->second;
            best_node_info = nodes_.at({ node_id, best_peer_session.lock()->GetSessionId() }).node_info;
        }

        for (auto it = nodes_.lower_bound({ node_id, 0 }); it != nodes_.end(); it++) {
            auto& [key, node_item] = *it;

            if (key.first != node_id) {
                break;
            }

            if (best_peer_session.lock() == nullptr) {
                is_updated = true;
                best_peer_session = node_item.peer_session.lock();
                best_node_info = nodes_.at({ node_id, best_peer_session.lock()->GetSessionId() }).node_info;
                continue;
            }

            /**
             * Algorithm to select best peering session
             * TODO(tievens): Add more advance selectors, such as load, geo distance, ...
             *
             * Choose the node that first matches the below in the order defined:
             *
             * 1. Prefer lower size `path` (hops)
             * 2. Prefer lower total sRTT
             *
             */
            if (best_node_info.path.size() > node_item.node_info.path.size() ||
                best_node_info.SumSrtt() > node_item.node_info.SumSrtt()) {
                is_updated = true;
                best_peer_session = node_item.peer_session.lock();
                best_node_info = nodes_.at({ node_id, best_peer_session.lock()->GetSessionId() }).node_info;
            }
        }

        if (is_updated) {
            nodes_best_[node_id] = best_peer_session;

            SPDLOG_DEBUG("Forwarding Table Dump BEGIN ------------------------------");

            // DEBUG - Dump the best nodes and their peering sessions to reach them
            for (const auto& node : nodes_best_) {
                auto node_info = node.second.lock();
                auto node_item = nodes_.at({ node.first, node_info->GetSessionId() });
                SPDLOG_DEBUG("Forwarding table node id: {} contact {} best via peer_session id: {} contact: {} "
                             "path_len: {} sum_srtt: {}",
                             node.first,
                             node_item.node_info.contact,
                             node_info->GetSessionId(),
                             node_info->remote_node_info_.contact,
                             node_item.node_info.path.size(),
                             node_item.node_info.SumSrtt());
            }
            SPDLOG_DEBUG("Forwarding Table Dump DONE ------------------------------");
        }

        return is_updated;
    }
}
