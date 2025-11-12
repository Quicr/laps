// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "info_base.h"
#include <quicr/hash.h>

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
            // TODO(tievens): Update subscribes and announces based on best change
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

        std::vector<std::pair<PeerSessionId, SubscribeNodeSetId>> fib_entries;
        for (auto it = peer_fib_.lower_bound({ peer_session_id, 0 }); it != peer_fib_.end(); it++) {
            auto& [key, fib_entry] = *it;

            if (key.first != peer_session_id) {
                break;
            }

            fib_entries.push_back(key);
        }

        for (const auto& key : fib_entries) {
            peer_fib_.erase(key);
        }
    }

    bool InfoBase::HasSubscribers(const SubscribeInfo& subscribe_info)
    {
        auto it = subscribes_.find(subscribe_info.track_hash.track_fullname_hash);
        if (it != subscribes_.end()) {
            for (const auto& si : it->second) {
                if (si.second.source_node_id != subscribe_info.source_node_id) {
                    return true;
                }
            }
        }

        return false;
    }

    bool InfoBase::AddSubscribe(const SubscribeInfo& subscribe_info)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_[subscribe_info.track_hash.track_fullname_hash].find(subscribe_info.source_node_id);
        if (it != subscribes_[subscribe_info.track_hash.track_fullname_hash].end()) {
            const int seq_diff = it->second.seq - subscribe_info.seq;
            if (seq_diff == 0) {
                // TODO(tievens): Revisit to check on order of received or delayed messages
                return false; // ignore
            }

            it->second = subscribe_info;
            return true;
        }

        subscribes_[subscribe_info.track_hash.track_fullname_hash].emplace(subscribe_info.source_node_id,
                                                                           subscribe_info);
        return true;
    }

    bool InfoBase::RemoveSubscribe(const SubscribeInfo& subscribe_info)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_.find(subscribe_info.track_hash.track_fullname_hash);
        if (it != subscribes_.end()) {
            auto sub_it = it->second.find(subscribe_info.source_node_id);
            if (sub_it != it->second.end()) {
                // TODO(tievens): Revisit to check on order of received or delayed messages
                it->second.erase(sub_it);

                if (it->second.empty()) {
                    subscribes_.erase(it);
                }

                return true;
            }
        }

        return false;
    }

    std::optional<SubscribeInfo> InfoBase::GetSubscribe(uint64_t track_fullname_hash, NodeIdValueType src_node_id)
    {
        std::lock_guard _(mutex_);

        auto it = subscribes_[track_fullname_hash].find(src_node_id);
        if (it != subscribes_[track_fullname_hash].end()) {
            return it->second;
        }

        return std::nullopt;
    }

    std::vector<std::size_t> InfoBase::PrefixHashNamespaceTuples(const quicr::TrackNamespace& name_space)
    {
        const auto& entries = name_space.GetHashes();
        std::vector<std::size_t> hashes(entries.size());

        uint64_t hash = 0;
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            quicr::hash_combine(hash, hashes[i]);
            hashes[i] = hash;
        }

        return hashes;
    }

    bool InfoBase::AddAnnounce(const AnnounceInfo& announce_info)
    {
        std::lock_guard _(mutex_);

        auto [__, is_new] =
          announces_[announce_info.fullname_hash].emplace(announce_info.source_node_id, announce_info);

        if (is_new) {
            for (const auto& prefix_hash : PrefixHashNamespaceTuples(announce_info.name_space)) {
                const auto it = prefix_lookup_announces_.find(prefix_hash);
                if (it == prefix_lookup_announces_.end()) {
                    prefix_lookup_announces_.emplace(prefix_hash, std::unordered_set{ announce_info.fullname_hash });
                } else {
                    if (auto s_it = it->second.find(announce_info.fullname_hash); s_it != it->second.end()) {
                        it->second.emplace(announce_info.fullname_hash);
                    }
                }
            }
        }
        // TODO: If not new, update metrics in existing entry

        return is_new;
    }

    bool InfoBase::RemoveAnnounce(const AnnounceInfo& announce_info)
    {
        bool removed{ false };
        std::lock_guard _(mutex_);

        auto it = announces_.find(announce_info.fullname_hash);
        if (it != announces_.end()) {
            removed = it->second.erase(announce_info.source_node_id) ? true : false;

            if (removed) {
                std::vector<uint64_t> remove_prefix_hashes;

                for (const auto& prefix_hash : PrefixHashNamespaceTuples(announce_info.name_space)) {
                    const auto it = prefix_lookup_announces_.find(prefix_hash);
                    if (it == prefix_lookup_announces_.end()) {
                        continue;
                    }

                    // remove announce full hash name from each prefix tuple set
                    if (auto s_it = it->second.find(announce_info.fullname_hash); s_it != it->second.end()) {
                        it->second.erase(s_it);

                        if (it->second.empty()) {
                            remove_prefix_hashes.push_back(prefix_hash);
                        }
                    }
                }

                // clean up the prefix lookup announces map
                for (const auto prefix_hash : remove_prefix_hashes) {
                    prefix_lookup_announces_.erase(prefix_hash);
                }
            }

            if (it->second.empty()) {
                announces_.erase(it);
            }
        }

        return removed;
    }

    std::set<NodeIdValueType> InfoBase::GetAnnounceIds(quicr::messages::TrackNamespace name_space,
                                                       quicr::messages::TrackName name,
                                                       bool exact)
    {
        std::set<NodeIdValueType> announces_ids;
        std::lock_guard _(mutex_);

        // Attempt to get a full match on namespace and/or namespace + name
        const auto th = quicr::TrackHash({ name_space, name });
        auto it = announces_.find(th.track_fullname_hash);
        if (it != announces_.end()) {
            for (const auto& a_info : it->second) {
                announces_ids.emplace(it->first);
            }
        }

        if (!announces_ids.empty() || exact)
            return announces_ids;

        // prefix match
        const auto prefix_hashes = PrefixHashNamespaceTuples(name_space);

        for (auto it = prefix_hashes.rbegin(); it != prefix_hashes.rend(); ++it) {
            auto p_it = prefix_lookup_announces_.find(*it);
            if (p_it != prefix_lookup_announces_.end()) {
                for (auto f_hash : p_it->second) {
                    auto it = announces_.find(f_hash);
                    if (it != announces_.end()) {
                        for (const auto& a_info : it->second) {
                            announces_ids.emplace(it->first);
                        }

                        if (!announces_ids.empty()) {
                            return announces_ids;
                        }
                    }
                }

                break;
            }
        }

        return announces_ids;
    }

    std::weak_ptr<PeerSession> InfoBase::GetBestPeerSession(NodeIdValueType node_id)
    {
        std::lock_guard _(mutex_);

        auto it = nodes_best_.find(node_id);
        if (it != nodes_best_.end()) {
            return it->second;
        }
        return {};
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
                             NodeId().Value(node.first),
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
