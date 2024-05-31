
#include "peer_manager.h"
#include <quicr/quicr_client.h>
#include <sstream>
#include <chrono>

namespace laps {

    PeerManager::PeerManager(const Config& cfg,
                             safe_queue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions)
      : _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
      , logger(std::make_shared<cantina::Logger>("PMGR", cfg.logger))
#ifndef LIBQUICR_WITHOUT_INFLUXDB
      , _mexport(std::make_shared<MetricsExporter>(logger))
#endif
    {
        _client_rx_msg_thr = std::thread(&PeerManager::PeerQueueThread, this);

        // TODO: Add config variable for interval_ms
        _watch_thr = std::thread(&PeerManager::watchThread, this, 30000);

        FLOG_INFO("Peering manager ID: " << _config.peer_config.id.c_str());

        TransportRemote server {
            .host_or_ip = cfg.peer_config.bind_addr,
            .port = cfg.peer_config.listen_port,
            .proto = cfg.peer_config.protocol == RelayInfo::Protocol::UDP ? TransportProtocol::UDP : TransportProtocol::QUIC
        };

        TransportConfig tconfig {
            .tls_cert_filename = _config.tls_cert_filename.c_str(),
            .tls_key_filename = _config.tls_key_filename.c_str(),
            .time_queue_init_queue_size = _config.data_queue_size,
            .time_queue_max_duration = 5000,
            .time_queue_bucket_interval = 2,
            .time_queue_rx_size = _config.rx_queue_size,
            .debug = _config.debug,
            .quic_cwin_minimum = static_cast<uint64_t>(_config.cwin_min_kb * 1024),
            .use_reset_wait_strategy = _config.use_reset_wait_strategy,
            .use_bbr = _config.use_bbr,
            .quic_qlog_path = _config.qlog_path.size() ? _config.qlog_path.c_str() : nullptr
        };

        _server_transport = qtransport::ITransport::make_server_transport(server, tconfig, *this, logger);
#ifndef LIBQUICR_WITHOUT_INFLUXDB
        _server_transport->start(_mexport->metrics_conn_samples, _mexport->metrics_data_samples);
#endif

        while (_server_transport->status() == qtransport::TransportStatus::Connecting) {
            logger->Log("Waiting for server to be ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

#ifndef LIBQUICR_WITHOUT_INFLUXDB
        if (_mexport->init("http://metrics.m10x.ctgpoc.com:8086",
                          "Media10x",
                          "cisco-cto-media10x") !=
            MetricsExporter::MetricsExporterError::NoError) {
            throw std::runtime_error("Failed to connect to InfluxDB");
        }

        if (!_server_transport->metrics_conn_samples) {
            logger->error << "ERROR metrics conn samples null" << std::flush;
        }
        _mexport->run();
#endif

        for (const auto& peer : cfg.peer_config.peers) {
            TransportRemote remote{ peer,
                                    cfg.peer_config.peer_port,
                                    cfg.peer_config.protocol == RelayInfo::Protocol::UDP ? TransportProtocol::UDP
                                                                                         : TransportProtocol::QUIC };
            createPeerSession(remote);
        }
    }

    PeerManager::~PeerManager()
    {
        // Stop threads
        _stop = true;

        FLOG_INFO("Closing peer manager threads");

        _client_peer_sessions.clear();
        _server_peer_sessions.clear();

        // Clear threads from the queue
        _peer_queue.stop_waiting();

        if (_client_rx_msg_thr.joinable())
            _client_rx_msg_thr.join();

        if (_watch_thr.joinable())
            _watch_thr.join();

        FLOG_INFO("Closed peer manager stopped");
    }

    void PeerManager::createPeerSession(const TransportRemote& peer_config)
    {
        auto &peer_sess = _client_peer_sessions.emplace_back(false, 0,
                                                             _config,
                                                             peer_config,
                                                             _peer_queue,
                                                             _cache,
                                                             _subscriptions
#ifndef LIBQUICR_WITHOUT_INFLUXDB
                                                            , _mexport
#endif
                                                             );

        peer_sess.connect();
    }

    void PeerManager::watchThread(int interval_ms)
    {
        FLOG_INFO("Running peer manager outbound peer connection thread");

        if (interval_ms < 2000)
            interval_ms = 2000;

        auto start = std::chrono::system_clock::now();

        while (not _stop) {
            auto check = std::chrono::system_clock::now();

            // Run check only after interval ms
            if (std::chrono::duration_cast<std::chrono::milliseconds>(check - start).count() >= interval_ms) {
                start = std::chrono::system_clock::now();

                for (auto& sess : _client_peer_sessions) {
                    if (sess.status() == PeerSession::Status::DISCONNECTED) {
                        FLOG_INFO("Peer session to " << sess.peer_config.host_or_ip.c_str()
                                                     << " disconnected, reconnecting");

                        sess.connect();
                    }
                }
            }

            // Sleep shorter so that the loop can be stopped within this time instead of a larger interval
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void PeerManager::addSubscribedPeer(const Namespace& ns, const peer_id_t& peer_id) {
        auto peer_subs_it = _peer_sess_subscribe_sent.find(ns);
        if (peer_subs_it != _peer_sess_subscribe_sent.end()) {
            peer_subs_it->second.insert(peer_id);
        } else {
            _peer_sess_subscribe_sent.emplace(ns, std::initializer_list<peer_id_t>({ peer_id }));
        }
    }

    PeerSession* PeerManager::getPeerSession(const peer_id_t& peer_id) {

        for (auto& sess : _client_peer_sessions) {
            if (!peer_id.compare(sess.getPeerId())) {
                return &sess;
            }
        }

        for (auto& [_, sess] : _server_peer_sessions) {
            if (!peer_id.compare(sess.getPeerId())) {
                return &sess;
            }
        }

        return nullptr;
    }


    void PeerManager::subscribePeers(const Namespace& ns, const peer_id_t& source_peer_id)
    {
        FLOG_DEBUG("Subscribe from " << source_peer_id << " to peers for ns: " << ns);

        auto it = _peer_sess_subscribe_recv.find(ns);
        if (it == _peer_sess_subscribe_recv.end() || it->second.empty()) {

            // No peers subscribed, check client edge subscription
            std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list = _subscriptions.find(ns);

            if (list.empty()) {
                FLOG_DEBUG("No subscribers, not sending subscription to peer(s) for " << ns);
                // No subscribers, do not send subscription
                return;
            }
        }

        // Add namespace with empty list if it doesn't exist
        auto peer_subs_it = _peer_sess_subscribe_sent.find(ns);
        if (peer_subs_it == _peer_sess_subscribe_sent.end()) {
            auto [it, _] = _peer_sess_subscribe_sent.emplace(ns, std::initializer_list<peer_id_t>({ }));
            peer_subs_it = it;
        }

        /*
         * When building the list of peers to send subscribe, only add first/front peer for given origin (best peer)
         * TODO: Update this to be more dynamic and adaptive
         */
        const auto iter = _pub_intent_namespaces.find(ns);
        std::vector<peer_id_t> peers;
        if (iter != _pub_intent_namespaces.end()) {
            for (const auto& [origin_peer, source_peers] : iter->second) {
                peers.push_back(source_peers.front());
            }
        }

        /*
         * Send subscribe towards best publish intent peers (that can reach origin)
         */
        for (const auto& peer_id : peers) {

            // Don't send back to same peer that sent it
            if (source_peer_id.compare(peer_id) == 0)
                continue;

            // Only send subscribe if subscribe wasn't already sent to this peer
            if (!peer_subs_it->second.contains(peer_id)) {
                peer_subs_it->second.insert(peer_id);

                auto sess = getPeerSession(peer_id);
                if (sess) {
                    sess->sendSubscribe(ns);
                    addSubscribedPeer(ns, peer_id);
                }
            } else {
                FLOG_DEBUG("Subscription " << ns << " already sent, suppressing");
            }
        }
    }

    void PeerManager::subscribePeer(const Namespace& ns, const peer_id_t& peer_id)
    {
        auto peer_subs_it = _peer_sess_subscribe_sent.find(ns);

        bool do_sub { false };

        if (peer_subs_it != _peer_sess_subscribe_sent.end()) {
            if (!peer_subs_it->second.contains(peer_id)) {
                peer_subs_it->second.insert(peer_id);
                do_sub = true;
            }
        } else {
            _peer_sess_subscribe_sent.emplace(ns, std::initializer_list<peer_id_t>({ peer_id }));
            do_sub = true;
        }

        if (do_sub) {
            auto sess = getPeerSession(peer_id);
            if (sess) {
                sess->sendSubscribe(ns);
            }
        }
    }

    void PeerManager::unSubscribePeer(const Namespace& ns, const peer_id_t& peer_id)
    {
        auto sess = getPeerSession(peer_id);
        if (sess) {
            sess->sendUnsubscribe(ns);
        }

        auto peer_subs_ns = _peer_sess_subscribe_sent.find(ns);

        if (peer_subs_ns != _peer_sess_subscribe_sent.end()) {
            peer_subs_ns->second.erase(peer_id);

            if (peer_subs_ns->second.size() == 0) {
                _peer_sess_subscribe_sent.erase(peer_subs_ns);
            }
        }
    }

    void PeerManager::unSubscribePeers(const Namespace& ns, const peer_id_t& source_peer_id)
    {
        for (const auto& [sub_ns, peers] : _peer_sess_subscribe_sent) {

            if (sub_ns != ns) {
                continue;
            }

            for (auto& peer_id : peers) {
                // Don't send back to same peer that sent it
                if (source_peer_id.compare(peer_id) == 0)
                    continue;

                auto sess = getPeerSession(peer_id);
                if (sess) {
                    sess->sendUnsubscribe(ns);
                }
            }
        }

        _peer_sess_subscribe_sent.erase(ns);
    }

    void PeerManager::publishIntentPeers(const Namespace& ns, const peer_id_t& source_peer_id,
                                         const peer_id_t& origin_peer_id)
    {
        bool update_peers { false };

        const auto iter = _pub_intent_namespaces.find(ns);
        if (iter == _pub_intent_namespaces.end()) {
            FLOG_DEBUG("New published intent message origin " << origin_peer_id);
            update_peers = true;

            std::map<peer_id_t, std::list<peer_id_t>> intent_map;
            intent_map.emplace(origin_peer_id, std::initializer_list<peer_id_t>{ source_peer_id });
            _pub_intent_namespaces.try_emplace(ns, intent_map);

        } else {
            const auto it = iter->second.find(origin_peer_id);
            if (it == iter->second.end()) {
                FLOG_DEBUG("New published intent origin " << origin_peer_id);
                update_peers = true;

                iter->second.emplace(origin_peer_id,
                                         std::initializer_list<peer_id_t>{ source_peer_id });
            } else {
                bool found{ false };
                for (const auto& peer_id : it->second) {
                    if (!peer_id.compare(source_peer_id)) {
                        found = true;
                        break;
                    }
                }

                if (not found) {
                    if (!origin_peer_id.compare(source_peer_id))
                        it->second.push_front(source_peer_id);
                    else
                        it->second.push_back(source_peer_id);
                }
            }
        }

        if (update_peers) {
            for (auto& sess : _client_peer_sessions) {
                if (source_peer_id.compare(sess.getPeerId()))
                    sess.sendPublishIntent(ns, origin_peer_id);
            }

            for (auto& [_, sess] : _server_peer_sessions) {
                if (source_peer_id.compare(sess.getPeerId()))
                    sess.sendPublishIntent(ns, origin_peer_id);
            }
        }
    }

    void PeerManager::publishIntentDonePeers(const Namespace& ns, const peer_id_t& source_peer_id,
                                             const peer_id_t& origin_peer_id)
    {
        const auto iter = _pub_intent_namespaces.find(ns);
        if (iter != _pub_intent_namespaces.end()) {
            FLOG_DEBUG("Removing publish intent ns: " << ns << " origin " << origin_peer_id);
            const auto it = iter->second.find(origin_peer_id);
            if (it != iter->second.end()) {
                it->second.remove(source_peer_id);

                if (it->second.size() == 0) {
                    iter->second.erase(it);
                }

                if (iter->second.size() == 0) {
                    _pub_intent_namespaces.erase(iter);
                }
            }
        }

        for (auto& sess: _client_peer_sessions) {
            if (source_peer_id.compare(sess.getPeerId()))
                sess.sendPublishIntentDone(ns, origin_peer_id);
        }

        for (auto& [_, sess]: _server_peer_sessions) {
            if (source_peer_id.compare(sess.getPeerId()))
                sess.sendPublishIntentDone(ns, origin_peer_id);
        }

    }

    void PeerManager::PeerQueueThread()
    {
        FLOG_INFO("Running peer manager queue receive thread");

        while (not _stop) {
            const auto& obj = _peer_queue.block_pop();

            if (obj) {
                switch (obj.value().type) {
                    case PeerObjectType::PUBLISH: {

                        for (auto& sess: _client_peer_sessions) {
                            if (obj->source_peer_id.compare(sess.getPeerId()))
                                sess.publishObject(obj->pub_obj);
                        }

                        for (auto& [_, sess]: _server_peer_sessions) {
                            if (obj->source_peer_id.compare(sess.getPeerId()))
                                sess.publishObject(obj->pub_obj);
                        }

                        break;
                    }

                    case PeerObjectType::SUBSCRIBE: {

                        if (obj->source_peer_id.compare(CLIENT_PEER_ID)) { // if from peer
                            FLOG_DEBUG("Received CMGR subscribe message name: " << obj->nspace);
                            _peer_sess_subscribe_recv.emplace(obj->nspace, std::initializer_list<peer_id_t>({obj->source_peer_id}));

                        } else {
                            FLOG_DEBUG("Received subscribe message name: " << obj->nspace << " source peer_id: " << obj->source_peer_id);
                        }

                        // Send subscribe to all peers toward the best publish intent peer(s)
                        subscribePeers(obj->nspace, obj->source_peer_id);

                        break;
                    }

                    case PeerObjectType::UNSUBSCRIBE: {

                        bool un_sub_all { false };

                        if (! obj->source_peer_id.compare(CLIENT_PEER_ID)) {
                            FLOG_DEBUG("Received CMGR unsubscribe message name: " << obj->nspace);

                            std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list =
                              _subscriptions.find(obj->nspace);

                            if (list.empty()) { // No clients, now check received
                                auto it = _peer_sess_subscribe_recv.find(obj->nspace);
                                if (it == _peer_sess_subscribe_recv.end() || it->second.empty()) {
                                    un_sub_all = true;
                                }
                            }

                        } else {
                            FLOG_DEBUG("Received unsubscribe message name: " << obj->nspace << " source peer_id: " << obj->source_peer_id);

                            // Only unsubscribe all peers if there are no other peers subscribed
                            auto it = _peer_sess_subscribe_recv.find(obj->nspace);
                            if (it != _peer_sess_subscribe_recv.end()) {

                                it->second.erase(obj->source_peer_id);

                                if (it->second.empty()) {
                                    un_sub_all = true;
                                    _peer_sess_subscribe_recv.erase(it);
                                } else {
                                    un_sub_all = false;
                                    logger->debug << "Peers " << it->second.size()
                                                  << " still subscribed to " << obj->nspace << std::flush;
                                }

                            } else {
                                un_sub_all = true;
                            }
                        }

                        if (un_sub_all) {
                            unSubscribePeers(obj->nspace, obj->source_peer_id);
                        }

                        break;
                    }

                    case PeerObjectType::PUBLISH_INTENT: {
                        peer_id_t origin_peer_id = obj->origin_peer_id;

                        if (! obj->source_peer_id.compare(CLIENT_PEER_ID)) {
                            origin_peer_id = _config.peer_config.id;
                            FLOG_DEBUG("Received CMGR publish intent message name: " << obj->nspace);
                        }

                        FLOG_DEBUG("Received publish intent message name: " << obj->nspace
                                                                            << " origin: " << origin_peer_id
                                                                            << " source peer_id: " << obj->source_peer_id);

                        publishIntentPeers(obj->nspace, obj->source_peer_id, origin_peer_id);

                        subscribePeers(obj->nspace, "_sync_"); // do not send src for intent based subscribes
                        break;
                    }

                    case PeerObjectType::PUBLISH_INTENT_DONE: {
                        peer_id_t origin_peer_id = obj->origin_peer_id;

                        if (! obj->source_peer_id.compare(CLIENT_PEER_ID)) {
                            origin_peer_id = _config.peer_config.id;
                            FLOG_DEBUG("Received CMGR publish intent done message name: " << obj->nspace);

                        } else {
                            FLOG_DEBUG("Received publish intent done message name: " << obj->nspace
                                                                                     << " origin: " << origin_peer_id
                                                                                     << " source peer_id: " << obj->source_peer_id);
                        }

                        publishIntentDonePeers(obj->nspace, obj->source_peer_id, origin_peer_id);

                        unSubscribePeer(obj->nspace, obj->source_peer_id);

                        break;
                    }
                }
            }
        }
    }

    /*
     * Delegate Implementations
     */
    void PeerManager::on_connection_status(const TransportConnId& conn_id, const TransportStatus status) {

        auto peer_iter = _server_peer_sessions.find(conn_id);

        switch (status) {
            case TransportStatus::Ready: {
                // nothing to do for incoming connections
                break;
            }

            case TransportStatus::Disconnected: {
                if (peer_iter == _server_peer_sessions.end())
                    break;

                FLOG_INFO("Peer conn_id: " << conn_id << " is disconnected, closing peer connection");

                _server_peer_sessions.erase(peer_iter);
            }
            default: {
                // ignored
            }
        }
    }

    void PeerManager::on_new_connection(const TransportConnId& conn_id, const TransportRemote& remote) {
        auto peer_iter = _server_peer_sessions.find(conn_id);

        if (peer_iter == _server_peer_sessions.end()) {
            FLOG_INFO("New server accepted peer, conn_id: " << conn_id);

            TransportRemote peer = remote;
            auto [iter, inserted] = _server_peer_sessions.try_emplace(conn_id,
                                                          true,
                                                          conn_id,
                                                          _config,
                                                          std::move(peer),
                                                          _peer_queue,
                                                          _cache,
                                                          _subscriptions
#ifndef LIBQUICR_WITHOUT_INFLUXDB
                                                          , _mexport
#endif
            );

            peer_iter = iter;

            auto& peer_sess = peer_iter->second;

            FLOG_INFO("Peer conn_id: " << conn_id << " is ready");

            peer_sess.setTransport(_server_transport);
            peer_sess.connect();


            for (const auto& [ns, origins]: _pub_intent_namespaces) {
                for (const auto& [o, l]: origins) {
                    if (peer_sess.getPeerId().compare(o))
                        peer_sess.sendPublishIntent(ns, o);
                }
            }
        }

    }

    void PeerManager::on_recv_stream(const TransportConnId& conn_id,
                                     uint64_t stream_id,
                                     std::optional<DataContextId> data_ctx_id,
                                     const bool is_bidir)
    {
        auto peer_iter = _server_peer_sessions.find(conn_id);
        if (peer_iter != _server_peer_sessions.end()) {
            peer_iter->second.on_recv_stream(conn_id, stream_id, data_ctx_id, is_bidir);
        }
    }

    void PeerManager::on_recv_dgram(const TransportConnId& conn_id, std::optional<DataContextId> data_ctx_id)
    {
        auto peer_iter = _server_peer_sessions.find(conn_id);
        if (peer_iter != _server_peer_sessions.end()) {
            peer_iter->second.on_recv_dgram(conn_id, data_ctx_id);
        }
    }

} // namespace laps
