// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#include "peer_manager.h"
#include "client_manager.h"
#include "messages/data_object.h"
#include "state.h"

#include <chrono>
#include <sstream>

namespace laps::peering {

    // ------------------------------------------------------------
    // Methods used by peer session to feedback info and actions
    // ------------------------------------------------------------

    void PeerManager::NodeReceived(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Node peer_session_id: {} received id: {} contact: {} should delete = {}",
                           peer_session_id,
                           NodeId().Value(node_info.id),
                           node_info.contact,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);

        if (not withdraw) {
            if (info_base_->AddNode(peer_session, node_info)) {
                // Add peer to path before advertising node
                auto adv_node_info = node_info;
                adv_node_info.path.push_back({ adv_node_info.id, peer_session->metrics_.srtt_us });
                PropagateNodeInfo(adv_node_info);
            }
        } else {
            info_base_->RemoveNode(peer_session_id, node_info.id);

            auto adv_node_info = node_info;
            adv_node_info.path.push_back({ peer_session->node_info_.id, peer_session->metrics_.srtt_us });
            PropagateNodeInfo(adv_node_info, true);
        }
    } catch (const std::exception&) {
        SPDLOG_LOGGER_DEBUG(LOGGER,
                            "Cannot find peer session {} to process node info received id: {} contact: {}",
                            peer_session_id,
                            NodeId().Value(node_info.id),
                            node_info.contact);
    }

    void PeerManager::SubscribeInfoReceived(PeerSessionId peer_session_id, SubscribeInfo& subscribe_info, bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Subscribe info received peer_session_id: {} fullname: {} namespace: {} withdraw: {}",
                           peer_session_id,
                           subscribe_info.track_hash.track_fullname_hash,
                           subscribe_info.track_hash.track_namespace_hash,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);

        const auto is_updated =
          withdraw ? info_base_->RemoveSubscribe(subscribe_info) : info_base_->AddSubscribe(subscribe_info);

        if (is_updated) {
            for (const auto& sess : client_peer_sessions_) {
                if (peer_session_id != sess.first)
                    sess.second->SendSubscribeInfo(subscribe_info, withdraw);
            }

            for (const auto& sess : server_peer_sessions_) {
                if (peer_session_id != sess.first)
                    sess.second->SendSubscribeInfo(subscribe_info, withdraw);
            }
        }

        /*
         * On subscribe, check if there are client announcements that match it. If so,
         * find best peer session and update peer session with subscribe node id
         * and send notification to client manager that there is a subscribe.
         */
        if (not withdraw) {
            uint64_t update_ref = rand();

            std::lock_guard _(state_.state_mutex);
            auto it = state_.announce_active.lower_bound({ subscribe_info.track_hash.track_namespace_hash, 0 });
            if (it != state_.announce_active.end()) {
                SPDLOG_LOGGER_INFO(
                  LOGGER, "Announce matched subscribe fullname: {}", subscribe_info.track_hash.track_fullname_hash);

                if (auto cm = client_manager_.lock()) {

                    quicr::SubscribeAttributes s_attrs;
                    s_attrs.priority = 10;

                    quicr::messages::MoqSubscribe sub;
                    subscribe_info.subscribe_data >> sub;

                    SPDLOG_LOGGER_INFO(LOGGER, "Subscribe to client manager track alias: {}", sub.track_alias);

                    cm->ProcessSubscribe(0,
                                         0,
                                         subscribe_info.track_hash,
                                         { sub.track_namespace, sub.track_name },
                                         quicr::messages::FilterType::LatestObject,
                                         s_attrs);
                }

                auto bp_it = info_base_->nodes_best_.find(subscribe_info.source_node_id);
                if (bp_it != info_base_->nodes_best_.end()) {
                    const auto& peer_session = bp_it->second.lock();
                    SPDLOG_LOGGER_DEBUG(
                      LOGGER,
                      "Best peer session for subscribe fullname: {} source_node: {} is via peer_session_id: {}",
                      subscribe_info.track_hash.track_fullname_hash,
                      subscribe_info.source_node_id,
                      peer_session->GetSessionId());

                    if (auto [sns_id, is_new] = peer_session->AddSubscribeSourceNode(
                          subscribe_info.track_hash.track_fullname_hash, subscribe_info.source_node_id);
                        is_new) {
                        SPDLOG_LOGGER_INFO(
                          LOGGER,
                          "New source added to peer session for subscribe fullname: {} source_node: {} is "
                          "via peer_session_id: {} sns_id: {}",
                          subscribe_info.track_hash.track_fullname_hash,
                          subscribe_info.source_node_id,
                          peer_session->GetSessionId(),
                          sns_id);

                        if (auto [_, is_new] = info_base_->client_fib_.try_emplace(
                              { subscribe_info.track_hash.track_fullname_hash, peer_session_id },
                              InfoBase::FibEntry{ update_ref, 0, sns_id, bp_it->second });
                            is_new) {
                            SPDLOG_LOGGER_INFO(LOGGER,
                                               "New subscribe fullname: {}, sending subscribe to client manager",
                                               subscribe_info.track_hash.track_fullname_hash);
                        }
                    }
                }
            }
        } else {
            auto bp_it = info_base_->nodes_best_.find(subscribe_info.source_node_id);
            if (bp_it != info_base_->nodes_best_.end()) {
                const auto& peer_session = bp_it->second.lock();
                if (const auto [_, sns_removed] = peer_session->RemoveSubscribeSourceNode(
                      subscribe_info.track_hash.track_fullname_hash, subscribe_info.source_node_id);
                    sns_removed) {
                    SPDLOG_LOGGER_INFO(LOGGER,
                                       "No subscribe nodes left via peer session {}, removed subscribe fullname: {}",
                                       peer_session->GetSessionId(),
                                       subscribe_info.track_hash.track_fullname_hash);

                    info_base_->client_fib_.erase({ subscribe_info.track_hash.track_fullname_hash, peer_session_id });

                    bool has_subscribe_peers{ false };
                    for (auto it =
                           info_base_->client_fib_.lower_bound({ subscribe_info.track_hash.track_fullname_hash, 0 });
                         it != info_base_->client_fib_.end();
                         ++it) {
                        auto& [key, _] = *it;

                        if (key.first != subscribe_info.track_hash.track_fullname_hash)
                            break;

                        has_subscribe_peers = true;
                        break;
                    }

                    if (not has_subscribe_peers) {
                        SPDLOG_LOGGER_INFO(LOGGER,
                                           "No peers left for subscribe fullname: {}, removing client subscribe state",
                                           subscribe_info.track_hash.track_fullname_hash);

                        if (auto cm = client_manager_.lock()) {
                            cm->RemovePublisherSubscribe(subscribe_info.track_hash);
                        }
                    }
                }
            }
        }

    } catch (const std::exception&) {
        // ignore
    }

    void PeerManager::AnnounceInfoReceived(PeerSessionId peer_session_id,
                                           const AnnounceInfo& announce_info,
                                           bool withdraw)
    try {
        SPDLOG_LOGGER_INFO(LOGGER,
                           "Announce info received peer_session_id: {} hash: {} withdraw: {}",
                           peer_session_id,
                           announce_info.full_name.full_name_hash,
                           withdraw);

        auto peer_session = GetPeerSession(peer_session_id);
        if (not withdraw && !info_base_->AddAnnounce(announce_info)) {
            // Don't send to other peers if we have seen this before (loop)
            return;
        }

        if (withdraw && !info_base_->RemoveAnnounce(announce_info)) {
            // Don't send to other peers if we don't have state (loop)
            return;
        }

        for (const auto& sess : client_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendAnnounceInfo(announce_info, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            if (peer_session_id != sess.first)
                sess.second->SendAnnounceInfo(announce_info, withdraw);
        }

    } catch (const std::exception&) {
        // ignore
    }

    void PeerManager::SessionChanged(PeerSessionId peer_session_id,
                                     PeerSession::StatusValue status,
                                     [[maybe_unused]] const NodeInfo& remote_node_info)
    {
        switch (status) {
            case PeerSession::StatusValue::kConnected: {
                SPDLOG_LOGGER_INFO(LOGGER, "Peer session connected peer_session_id: {}", peer_session_id);

                break;
            }
            case PeerSession::StatusValue::kConnecting:
                break;

            default: {
                SPDLOG_LOGGER_INFO(LOGGER, "Peer session not connected peer_session_id: {}", peer_session_id);

                PropagateNodeInfo(remote_node_info, true);

                if (!stop_)
                    info_base_->PurgePeerSessionInfo(peer_session_id);

                // Remove or find new best peer for subscribe info
                std::vector<SubscribeInfo> remove_sub;
                std::vector<std::pair<PeerSessionId, SubscribeInfo>> update_sub;
                std::unique_lock<std::mutex> ib_lock(info_base_->mutex_);
                for (const auto& subs : info_base_->subscribes_) {
                    auto sub_it = subs.second.find(remote_node_info.id);
                    if (sub_it != subs.second.end()) {
                        remove_sub.emplace_back(sub_it->second);
                    } else {
                        /*
                         * check if peer session was used as best path for subscription. If so, try to find another
                         *      best path. If no best path can be found, remove subscribe info.
                         */
                        for (auto& [_, si] : subs.second) {
                            auto cfib_it =
                              info_base_->client_fib_.find({ si.track_hash.track_fullname_hash, peer_session_id });
                            if (cfib_it != info_base_->client_fib_.end()) {
                                info_base_->client_fib_.erase(cfib_it);
                                auto best_peer = info_base_->GetBestPeerSession(si.source_node_id);
                                if (const auto& peer_sess = best_peer.lock()) {
                                    // best path found, update entry to use new path
                                    update_sub.emplace_back(peer_sess->GetSessionId(), si);
                                } else {
                                    // No best path found, remove
                                    remove_sub.emplace_back(sub_it->second);
                                }
                            }
                        }
                    }
                }
                ib_lock.unlock();

                for (auto& si : remove_sub) {
                    SubscribeInfoReceived(peer_session_id, si, true);
                }

                for (auto& [id, si] : update_sub) {
                    SubscribeInfoReceived(id, si, false);
                }

                // Remove all announces if no active peering sessions exists
                bool remove_announce{ true };
                for (const auto& [id, peer_sess] : client_peer_sessions_) {
                    if (peer_sess->Status() == PeerSession::StatusValue::kConnected) {
                        remove_announce = false;
                        break;
                    }
                }

                if (remove_announce) {
                    for (const auto& [id, peer_sess] : server_peer_sessions_) {
                        if (peer_sess->Status() == PeerSession::StatusValue::kConnected) {
                            remove_announce = false;
                            break;
                        }
                    }
                }

                if (remove_announce) {
                    info_base_->announces_.clear();
                }

                break;
            }
        }
    }

    void PeerManager::CompleteDataObjectReceived(PeerSessionId peer_session_id, const DataObject& data_object)
    {
        auto it = info_base_->peer_fib_.find({ peer_session_id, data_object.sns_id });
        if (it != info_base_->peer_fib_.end()) {
            if (it->second.count(0)) { // client manager is interested
                client_manager_.lock()->ProcessPeerDataObject(data_object);
            }
        }
    }

    void PeerManager::ForwardPeerData(PeerSessionId peer_session_id,
                                      uint64_t stream_id,
                                      SubscribeNodeSetId in_sns_id,
                                      uint8_t priority,
                                      uint32_t ttl,
                                      Span<uint8_t> data,
                                      quicr::ITransport::EnqueueFlags eflags)
    {
        std::lock_guard _(info_base_->mutex_); // TODO: See about removing this lock
        auto it = info_base_->peer_fib_.find({ peer_session_id, in_sns_id });
        if (it != info_base_->peer_fib_.end()) {
            for (auto& [out_peer_sess_id, entry] : it->second) {
                if (out_peer_sess_id == 0 || out_peer_sess_id == peer_session_id)
                    continue; // Skip; don't send back to same peer or if it's self

                if (stream_id < entry.stream_id)
                    continue; // Invalid, must be an old object

                entry.stream_id = stream_id;

                // Update SNS_ID if new stream header included or if datagram (both have sns_id)
                if (eflags.new_stream || eflags.use_reliable == false) {
                    auto sns_id_bytes = BytesOf(entry.out_sns_id);
                    std::copy(sns_id_bytes.rbegin(), sns_id_bytes.rend(), data.begin() + 2);
                }

                auto out_peer_sess = entry.peer_session.lock();
                out_peer_sess->SendData(priority, ttl, entry.out_sns_id, eflags, data);
            }
        }
    }

    void PeerManager::ClientDataObject(quicr::TrackFullNameHash track_full_name_hash,
                                       uint8_t priority,
                                       uint32_t ttl,
                                       quicr::messages::GroupId group_id,
                                       quicr::messages::SubGroupId sub_group_id,
                                       DataObjectType type,
                                       Span<uint8_t const> data)
    {
        DataObject data_object;
        data_object.type = type;
        data_object.priority = priority;
        data_object.ttl = ttl;
        data_object.group_id = group_id;
        data_object.sub_group_id = sub_group_id;
        data_object.data = data;
        data_object.data_length = data.size();
        data_object.track_full_name_hash = track_full_name_hash;

        auto net_data = data_object.Serialize();

        quicr::ITransport::EnqueueFlags eflags;

        bool set_sns_id{ false };
        switch (type) {
            case DataObjectType::kDatagram:
                eflags.use_reliable = false;
                set_sns_id = true;
                break;
            case DataObjectType::kExistingStream:
                eflags.use_reliable = true;
                break;
            case DataObjectType::kNewStream:
                set_sns_id = true;
                eflags.use_reliable = true;
                eflags.new_stream = true;
                eflags.clear_tx_queue = true;
                eflags.use_reset = true;
                break;
        }

        for (auto it = info_base_->client_fib_.lower_bound({ track_full_name_hash, 0 });
             it != info_base_->client_fib_.end();
             ++it) {
            const auto& fib_entry = it->second;

            if (it->first.first != track_full_name_hash)
                break;

            if (const auto peer_sess = fib_entry.peer_session.lock()) {
                if (set_sns_id) {
                    /*
                    SPDLOG_LOGGER_DEBUG(LOGGER,
                                        "Data object send, setting SNS_ID: {} tfn_hash: {}",
                                        fib_entry.sns_id,
                                        track_full_name_hash);
                    */
                    auto sns_id_bytes = BytesOf(fib_entry.out_sns_id);
                    std::copy(sns_id_bytes.rbegin(), sns_id_bytes.rend(), net_data.begin() + 2);
                }
                peer_sess->SendData(priority, ttl, fib_entry.out_sns_id, eflags, net_data);
            }
        }
    }

    void PeerManager::ClientSubscribe(const quicr::FullTrackName& track_full_name,
                                      const quicr::SubscribeAttributes&,
                                      Span<const uint8_t> subscribe_data,
                                      bool withdraw)
    {
        quicr::TrackHash th(track_full_name);

        SubscribeInfo si;

        si.track_hash = th;
        si.subscribe_data.assign(subscribe_data.begin(), subscribe_data.end());
        si.source_node_id = node_info_.id;

        if (not withdraw) {
            info_base_->AddSubscribe(si);
        } else {
            info_base_->RemoveSubscribe(si);
        }

        for (const auto& sess : client_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending subscribe fullname: {} peer_session_id: {}",
                                si.track_hash.track_fullname_hash,
                                sess.first);
            sess.second->SendSubscribeInfo(si, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending subscribe fullname: {} peer_session_id: {}",
                                si.track_hash.track_fullname_hash,
                                sess.first);
            sess.second->SendSubscribeInfo(si, withdraw);
        }
    }

    void PeerManager::ClientAnnounce(const quicr::FullTrackName& track_full_name,
                                     const quicr::PublishAnnounceAttributes&,
                                     bool withdraw)
    {
        AnnounceInfo ai;

        ai.full_name.full_name_hash = std::hash<quicr::TrackNamespace>{}(track_full_name.name_space);
        ai.full_name.namespace_tuples.reserve(track_full_name.name_space.GetHashes().size());
        ai.source_node_id = node_info_.id;

        if (not withdraw) {
            info_base_->AddAnnounce(ai);
        } else {
            info_base_->RemoveAnnounce(ai);
        }

        for (const auto ns_item : track_full_name.name_space.GetHashes()) {
            ai.full_name.namespace_tuples.push_back(ns_item);
        }

        ai.full_name.name_hash = 0;

        for (const auto& sess : client_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Sending announce hash: {} peer_session_id: {}", ai.full_name.full_name_hash, sess.first);
            sess.second->SendAnnounceInfo(ai, withdraw);
        }

        for (const auto& sess : server_peer_sessions_) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Sending announce hash: {} peer_session_id: {}", ai.full_name.full_name_hash, sess.first);
            sess.second->SendAnnounceInfo(ai, withdraw);
        }

        if (not withdraw) {
            uint64_t update_ref = rand();

            // TODO: change to not iterate over all subscribes to find a match
            for (const auto& [ftn, sub_map] : info_base_->subscribes_) {

                for (const auto si_it : sub_map) {
                    if (si_it.first == node_info_.id)
                        continue;
                    const auto& sub_info = si_it.second;

                    quicr::messages::MoqSubscribe sub;
                    sub_info.subscribe_data >> sub;

                    if (track_full_name.name_space.HasSamePrefix(sub.track_namespace)) {

                        if (sub_info.source_node_id == node_info_.id)
                            continue;

                        if (auto cm = client_manager_.lock()) {
                            quicr::SubscribeAttributes s_attrs;
                            s_attrs.priority = 10;

                            SPDLOG_LOGGER_INFO(LOGGER, "Subscribe to client manager track alias: {}", sub.track_alias);

                            cm->ProcessSubscribe(0,
                                                 0,
                                                 sub_info.track_hash,
                                                 { sub.track_namespace, sub.track_name, 0 },
                                                 quicr::messages::FilterType::LatestObject,
                                                 s_attrs);
                        }

                        auto bp_it = info_base_->nodes_best_.find(sub_info.source_node_id);
                        if (bp_it != info_base_->nodes_best_.end()) {
                            const auto& peer_session = bp_it->second.lock();
                            SPDLOG_LOGGER_DEBUG(
                              LOGGER,
                              "Best peer session for subscribe fullname: {} source_node: {} is via peer_session_id: {}",
                              sub_info.track_hash.track_fullname_hash,
                              sub_info.source_node_id,
                              peer_session->GetSessionId());

                            if (auto [sns_id, is_new] = peer_session->AddSubscribeSourceNode(
                                  sub_info.track_hash.track_fullname_hash, sub_info.source_node_id);
                                is_new) {
                                SPDLOG_LOGGER_INFO(
                                  LOGGER,
                                  "New source added to peer session for subscribe fullname: {} source_node: {} is "
                                  "via peer_session_id: {} sns_id: {}",
                                  sub_info.track_hash.track_fullname_hash,
                                  sub_info.source_node_id,
                                  peer_session->GetSessionId(),
                                  sns_id);

                                if (auto [_, is_new] = info_base_->client_fib_.try_emplace(
                                      { sub_info.track_hash.track_fullname_hash, peer_session->GetSessionId() },
                                      InfoBase::FibEntry{ update_ref, 0, sns_id, bp_it->second });
                                    is_new) {
                                    SPDLOG_LOGGER_INFO(LOGGER,
                                                       "New subscribe fullname: {} added to client fib",
                                                       sub_info.track_hash.track_fullname_hash);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------
    // Peer Manager
    // -------------------------------------------------------------

    PeerManager::PeerManager(const Config& cfg, State& state, std::shared_ptr<InfoBase> info_base)
      : info_base_(info_base)
      , tick_service_(std::make_shared<quicr::ThreadedTickService>())
      , config_(cfg)
      , state_(state)
    {
        SPDLOG_LOGGER_INFO(
          LOGGER, "Peering manager Node ID: {} listening port: {}", config_.relay_id_, config_.peering.listening_port);

        check_thr_ = std::thread(&PeerManager::CheckThread, this, cfg.peering.check_interval_ms);

        node_info_.contact = config_.relay_id_;
        node_info_.id = std::hash<std::string>{}(config_.relay_id_);
        node_info_.type = config_.node_type;

        quicr::TransportRemote server{ "0.0.0.0", config_.peering.listening_port, quicr::TransportProtocol::kQuic };

        quicr::TransportConfig tconfig;
        tconfig.debug = false; // cfg.debug;
        tconfig.tls_cert_filename = config_.tls_cert_filename_;
        tconfig.tls_key_filename = config_.tls_key_filename_;
        tconfig.time_queue_init_queue_size = config_.peering.init_queue_size;
        tconfig.time_queue_max_duration = config_.peering.max_ttl_expiry_ms;
        tconfig.idle_timeout_ms = 5000;

        server_transport_ =
          quicr::ITransport::MakeServerTransport(std::move(server), std::move(tconfig), *this, tick_service_, LOGGER);
        server_transport_->Start();

        while (server_transport_->Status() == quicr::TransportStatus::kConnecting) {
            SPDLOG_LOGGER_INFO(LOGGER, "Waiting for server to be ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (const auto& [host, port] : cfg.peering.peers) {
            quicr::TransportRemote remote{ host, port, quicr::TransportProtocol::kQuic };
            CreatePeerSession(std::move(remote));
        }
    }

    PeerManager::~PeerManager()
    {
        // Stop threads
        stop_ = true;

        SPDLOG_LOGGER_INFO(LOGGER, "Closing peer manager threads");

        client_peer_sessions_.clear();
        server_peer_sessions_.clear();

        if (check_thr_.joinable())
            check_thr_.join();

        SPDLOG_LOGGER_INFO(LOGGER, "Closed peer manager stopped");
    }

    void PeerManager::SnsReceived(PeerSession& peer_session, const SubscribeNodeSet& sns, bool withdraw)
    {
        uint64_t update_ref = rand();

        std::lock_guard _(mutex_);

        if (withdraw) {
            auto it = info_base_->peer_fib_.find({ peer_session.GetSessionId(), sns.id });
            if (it != info_base_->peer_fib_.end()) {
                for (const auto& [out_peer_sess_id, entry] : it->second) {
                    if (out_peer_sess_id == 0)
                        continue;

                    auto out_peer_sess = entry.peer_session.lock();
                    out_peer_sess->RemovePeerSnsSourceNode(peer_session.GetSessionId(), sns.id, 0);
                }

                info_base_->peer_fib_.erase(it);
            }

            return;
        }

        auto [fib_it, new_ingress] = info_base_->peer_fib_.try_emplace({ peer_session.GetSessionId(), sns.id });
        if (new_ingress) {
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "New ingress SNS peer session: {} sns id: {}", peer_session.GetSessionId(), sns.id);

            for (const auto& node_id : sns.nodes) {

                if (node_id == node_info_.id) {
                    // Self
                    fib_it->second[0] = InfoBase::FibEntry{ update_ref, {} };
                    continue;
                }

                auto peer_sess_weak = info_base_->GetBestPeerSession(node_id);
                if (auto peer_sess = peer_sess_weak.lock()) {

                    const auto [out_sns_id, out_new] =
                      peer_sess->AddPeerSnsSourceNode(peer_session.GetSessionId(), sns.id, node_id);

                    // Update or create fib record
                    fib_it->second[peer_sess->GetSessionId()] =
                      InfoBase::FibEntry{ update_ref, 0, out_sns_id, peer_sess_weak };
                }
            }
        }

        else { // Existing
            SPDLOG_LOGGER_DEBUG(
              LOGGER, "Update to ingress SNS peer session: {} sns id: {}", peer_session.GetSessionId(), sns.id);

            for (const auto& node_id : sns.nodes) {
                if (node_id == node_info_.id) {
                    // Self
                    fib_it->second[0] = InfoBase::FibEntry{ update_ref, {} };
                    continue;
                }

                auto peer_sess_weak = info_base_->GetBestPeerSession(node_id);

                if (auto peer_sess = peer_sess_weak.lock()) {
                    auto it = fib_it->second.find(peer_sess->GetSessionId());
                    if (it == fib_it->second.end()) {
                        // New entry
                        const auto [o_sns_id, __] =
                          peer_sess->AddPeerSnsSourceNode(peer_session.GetSessionId(), sns.id, node_id);

                        fib_it->second[peer_sess->GetSessionId()] =
                          InfoBase::FibEntry{ update_ref, 0, o_sns_id, peer_sess_weak };

                        SPDLOG_LOGGER_DEBUG(LOGGER,
                                            "SNS added peer session: {} sns id: {} added source node_id: {}",
                                            peer_sess->GetSessionId(),
                                            o_sns_id,
                                            NodeId().Value(node_id));

                    } else {
                        // Existing entry
                        const auto [o_sns_id, is_new] =
                          peer_sess->AddPeerSnsSourceNode(peer_session.GetSessionId(), sns.id, node_id);
                        if (is_new) {
                            SPDLOG_LOGGER_DEBUG(LOGGER,
                                                "SNS update peer session: {} sns id: {} added source node_id: {}",
                                                it->second.peer_session.lock()->GetSessionId(),
                                                o_sns_id,
                                                NodeId().Value(node_id));
                        }

                        fib_it->second[peer_sess->GetSessionId()] =
                          InfoBase::FibEntry{ update_ref, 0, o_sns_id, peer_sess_weak };
                    }
                }
            }

            // Not ideal, but iterate over map of peers to remove entries that were not updated
            std::vector<PeerSessionId> remove_peers;
            for (auto& [peer_sess_id, entry] : fib_it->second) {
                if (entry.update_ref == update_ref)
                    continue;

                SPDLOG_LOGGER_DEBUG(LOGGER,
                                    "SNS update remove peer session: {} sns id: {}",
                                    peer_sess_id,
                                    entry.out_sns_id);

                if (const auto peer_sess = entry.peer_session.lock()) {
                    peer_sess->RemovePeerSnsSourceNode(peer_sess->GetSessionId(), sns.id, 0);
                }

                remove_peers.push_back(peer_sess_id);
            }

            for (const auto& peer_sess_id : remove_peers) {
                fib_it->second.erase(peer_sess_id);
            }
        }
    }

    void PeerManager::InfoBaseSyncPeer(PeerSession& peer_session)
    {
        std::lock_guard _(mutex_);

        // Send all node info
        for (auto [key, node_item] : info_base_->nodes_) {
            if (key.first == peer_session.node_info_.id || key.second == peer_session.GetSessionId() ||
                key.first == peer_session.remote_node_info_.id)
                continue; // Skip, node is self, learned from peer, or peer is the node info

            bool skip{ false };
            for (auto& npi : node_item.node_info.path) {
                if (npi.id == peer_session.remote_node_info_.id) {
                    skip = true;
                    break; // Skip, remote peer is in path
                }
            }

            if (skip)
                continue;

            node_item.node_info.path.push_back({ peer_session.node_info_.id, peer_session.metrics_.srtt_us });
            peer_session.SendNodeInfo(node_item.node_info);
        }

        // Send all announces
        for (const auto& [th, anno_item] : info_base_->announces_) {
            for (const auto& [_, anno_info] : anno_item) {
                if (peer_session.remote_node_info_.id == anno_info.source_node_id)
                    continue; // Skip, don't send self or remote to self

                peer_session.SendAnnounceInfo(anno_info);
            }
        }

        // Send all subscribes
        for (auto& [tfh, sub_item] : info_base_->subscribes_) {
            for (auto& [_, sub_info] : sub_item) {
                if (peer_session.remote_node_info_.id == sub_info.source_node_id)
                    continue; // Skip, don't send self or remote to self

                peer_session.SendSubscribeInfo(sub_info);
            }
        }
    }

    void PeerManager::PropagateNodeInfo(PeerSessionId peer_session_id, const NodeInfo& node_info, bool withdraw)
    {
        std::lock_guard _(mutex_);

        auto server_it = server_peer_sessions_.find(peer_session_id);
        if (server_it != server_peer_sessions_.end()) {
            if (server_it->second->remote_node_info_.id != node_info.id)
                server_it->second->SendNodeInfo(node_info, withdraw);
            return;
        }

        auto client_it = client_peer_sessions_.find(peer_session_id);
        if (client_it != client_peer_sessions_.end()) {
            if (client_it->second->remote_node_info_.id != node_info.id)
                client_it->second->SendNodeInfo(node_info, withdraw);
            return;
        }
    }

    void PeerManager::PropagateNodeInfo(const NodeInfo& node_info, bool withdraw)
    {
        auto skip_node = [](const NodeInfo& node_a, const NodeInfo& node_b) -> bool {
            if (node_a.id == node_b.id)
                return true;

            for (auto& npi : node_a.path) {
                if (npi.id == node_b.id) {
                    return true;
                }
            }

            return false;
        };

        std::lock_guard _(mutex_);

        // Advertise node info to peer
        for (auto& [_, peer_session] : client_peer_sessions_) {
            if (skip_node(node_info, peer_session->remote_node_info_))
                continue;

            peer_session->SendNodeInfo(node_info, withdraw);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} contact: {} withdraw: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact,
                                withdraw);
        }

        for (auto& [_, peer_session] : server_peer_sessions_) {
            if (skip_node(node_info, peer_session->remote_node_info_))
                continue;

            peer_session->SendNodeInfo(node_info, withdraw);

            SPDLOG_LOGGER_DEBUG(LOGGER,
                                "Sending node info; id: {} contact: {} --> remote id: {} --> contact: {} withdraw: {}",
                                node_info.id,
                                node_info.contact,
                                peer_session->remote_node_info_.id,
                                peer_session->remote_node_info_.contact,
                                withdraw);
        }
    }

    inline std::shared_ptr<PeerSession> PeerManager::GetPeerSession(const PeerSessionId peer_session_id)
    {
        auto s_peer_it = server_peer_sessions_.find(peer_session_id);
        if (s_peer_it != server_peer_sessions_.end()) {
            return s_peer_it->second;
        }

        auto c_peer_it = client_peer_sessions_.find(peer_session_id);
        if (c_peer_it != client_peer_sessions_.end()) {
            return c_peer_it->second;
        }

        throw std::invalid_argument("Peer session id does not exist");
    }

    void PeerManager::CreatePeerSession(const quicr::TransportRemote& peer_config)
    {
        auto peer_sess = std::make_shared<PeerSession>(false, 0, config_, node_info_, peer_config, *this);
        peer_sess->Connect();

        client_peer_sessions_.try_emplace(peer_sess->GetSessionId(), std::move(peer_sess));
    }

    void PeerManager::CheckThread(int interval_ms)
    {
        SPDLOG_LOGGER_INFO(LOGGER, "Running peer manager outbound peer connection thread");

        if (interval_ms < 2000)
            interval_ms = 2000;

        auto start = std::chrono::system_clock::now();

        while (not stop_) {
            auto check = std::chrono::system_clock::now();

            // Run check only after interval ms
            if (std::chrono::duration_cast<std::chrono::milliseconds>(check - start).count() >= interval_ms) {
                start = std::chrono::system_clock::now();

                std::vector<PeerSessionId> remove_peer_sess;
                for (auto& [id, sess] : client_peer_sessions_) {
                    if (sess->Status() == PeerSession::StatusValue::kDisconnected) {
                        SPDLOG_LOGGER_INFO(LOGGER, "Peer session {} disconnected, reconnecting", sess->GetSessionId());
                        sess->Connect();
                        client_peer_sessions_.try_emplace(sess->GetSessionId(), std::move(sess));
                        remove_peer_sess.push_back(id); // New connect has new session ID, remove old
                    }
                }

                for (const auto id : remove_peer_sess) {
                    client_peer_sessions_.erase(id);
                }
            }

            // Sleep shorter so that the loop can be stopped within this time instead of a larger interval
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms / 2));
        }
    }

    /*
     * Delegate Implementations
     */
    void PeerManager::OnConnectionStatus(const quicr::TransportConnId& conn_id, const quicr::TransportStatus status)
    {
        auto peer_it = server_peer_sessions_.find(conn_id);
        if (peer_it == server_peer_sessions_.end()) {
            return;
        }

        PeerSession::StatusValue sess_status = PeerSession::StatusValue::kConnected;
        switch (status) {
            case quicr::TransportStatus::kReady: {
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is connected", conn_id);
                break;
            }
            case quicr::TransportStatus::kConnecting:
                break;

            case quicr::TransportStatus::kDisconnected: {
                sess_status = PeerSession::StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} is disconnected", conn_id);
                break;
            }

            case quicr::TransportStatus::kRemoteRequestClose:
                sess_status = PeerSession::StatusValue::kDisconnected;

                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} remote disconnected", conn_id);
                break;

            case quicr::TransportStatus::kShutdown:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;

            case quicr::TransportStatus::kIdleTimeout:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} idle timeout", conn_id);
                break;

            case quicr::TransportStatus::kShuttingDown:
                sess_status = PeerSession::StatusValue::kDisconnected;
                SPDLOG_LOGGER_DEBUG(LOGGER, "Peer conn_id {0} shutdown", conn_id);
                break;
        }

        SessionChanged(peer_it->second->GetSessionId(), sess_status, peer_it->second->remote_node_info_);

        server_peer_sessions_.erase(peer_it);
    }

    void PeerManager::OnNewConnection(const quicr::TransportConnId& conn_id, const quicr::TransportRemote& remote)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);

        if (peer_iter == server_peer_sessions_.end()) {
            SPDLOG_LOGGER_INFO(LOGGER, "New server accepted peer, conn_id: {0}", conn_id);

            quicr::TransportRemote peer = remote;
            auto [iter, inserted] = server_peer_sessions_.try_emplace(
              conn_id, std::make_shared<PeerSession>(true, conn_id, config_, node_info_, std::move(peer), *this));

            peer_iter = iter;

            auto& peer_sess = peer_iter->second;

            peer_sess->SetTransport(server_transport_);
            peer_sess->Connect();
        }
    }

    void PeerManager::OnRecvStream(const quicr::TransportConnId& conn_id,
                                   uint64_t stream_id,
                                   std::optional<quicr::DataContextId> data_ctx_id,
                                   const bool is_bidir)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);
        if (peer_iter != server_peer_sessions_.end()) {
            peer_iter->second->OnRecvStream(conn_id, stream_id, data_ctx_id, is_bidir);
        }
    }

    void PeerManager::OnRecvDgram(const quicr::TransportConnId& conn_id,
                                  std::optional<quicr::DataContextId> data_ctx_id)
    {
        auto peer_iter = server_peer_sessions_.find(conn_id);
        if (peer_iter != server_peer_sessions_.end()) {
            peer_iter->second->OnRecvDgram(conn_id, data_ctx_id);
        }
    }

} // namespace laps