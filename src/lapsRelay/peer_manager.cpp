
#include "peer_manager.h"
#include <quicr/quicr_client.h>
#include <sstream>
#include <chrono>

namespace laps {

    PeerManager::PeerManager(const Config& cfg,
                             safeQueue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions)
      : _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
    {
        logger = cfg.logger;

        _client_rx_msg_thr = std::thread(&PeerManager::PeerQueueThread, this);

        // TODO: Add config variable for interval_ms
        _watch_thr = std::thread(&PeerManager::watchThread, this, 30000);

        LOG_INFO("Peering manager ID: %s", _config.peer_config.id.c_str());

        TransportRemote server {
            .host_or_ip = cfg.peer_config.bind_addr,
            .port = cfg.peer_config.listen_port,
            .proto = cfg.peer_config.protocol == RelayInfo::Protocol::UDP ? TransportProtocol::UDP : TransportProtocol::QUIC
        };

        TransportConfig tconfig {
            .tls_cert_filename = _config.tls_cert_filename.c_str(),
            .tls_key_filename = _config.tls_key_filename.c_str(),
            .time_queue_init_queue_size = _config.data_queue_size,
            .time_queue_max_duration = 1000,
            .time_queue_bucket_interval = 2,
            .time_queue_rx_ttl = _config.time_queue_ttl_default
        };

        _server_transport = qtransport::ITransport::make_server_transport(server, tconfig, *this, *logger);
        _server_transport->start();

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

        LOG_INFO("Closing peer manager threads");

        _client_peer_sessions.clear();
        _server_peer_sessions.clear();

        // Clear threads from the queue
        _peer_queue.stopWaiting();

        if (_client_rx_msg_thr.joinable())
            _client_rx_msg_thr.join();

        if (_watch_thr.joinable())
            _watch_thr.join();

        LOG_INFO("Closed peer manager stopped");
    }

    void PeerManager::createPeerSession(const TransportRemote& peer_config)
    {
        auto &peer_sess = _client_peer_sessions.emplace_back(false, 0,
                                                             _config,
                                                             peer_config,
                                                             _peer_queue,
                                                             _cache,
                                                             _subscriptions);

        peer_sess.connect();
    }

    void PeerManager::watchThread(int interval_ms)
    {
        LOG_INFO("Running peer manager outbound peer connection thread");

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
                        LOG_INFO("Peer session to %s disconnected, reconnecting", sess.peer_config.host_or_ip.c_str());

                        sess.connect();
                    }
                }
            }

            // Sleep shorter so that the loop can be stopped within this time instead of a larger interval
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void PeerManager::subscribePeers(const Namespace& ns, const std::string& source_peer_id)
    {
        _peer_sess_subscribed.try_emplace(ns, source_peer_id);

        const auto& iter = _pub_intent_namespaces.find(ns);
        std::vector<std::string> peers;
        if (iter != _pub_intent_namespaces.end()) {
            for (const auto& p : iter->second) {
                peers.push_back(p.second.front());
            }
        }

        for (auto& sess : _client_peer_sessions) {
            for (const auto &p : peers) {
                if (!p.compare(sess.getPeerId())) {
                    sess.sendSubscribe(ns);
                }
            }
        }

        for (auto& [_, sess] : _server_peer_sessions) {
            for (const auto &p : peers) {
                if (!p.compare(sess.getPeerId())) {
                    sess.sendSubscribe(ns);
                }
            }
        }
    }

    void PeerManager::unSubscribePeers(const Namespace& ns, [[maybe_unused]] const std::string& source_peer_id)
    {
        auto iter = _peer_sess_subscribed.find(ns);
        if (iter != _peer_sess_subscribed.end()) {
            _peer_sess_subscribed.erase(iter);
        }

        const auto it = _pub_intent_namespaces.find(ns);
        std::vector<std::string> peers;
        if (it != _pub_intent_namespaces.end()) {
            for (const auto& p : it->second) {
                peers.push_back(p.second.front());
            }
        }

        for (auto& sess : _client_peer_sessions) {
            for (const auto &p : peers) {
                if (!p.compare(sess.getPeerId())) {
                    sess.sendUnsubscribe(ns);
                }
            }
        }

        for (auto& [_, sess] : _server_peer_sessions) {
            for (const auto &p : peers) {
                if (!p.compare(sess.getPeerId())) {
                    sess.sendUnsubscribe(ns);
                }
            }
        }

    }

    void PeerManager::publishIntentPeers(const Namespace& ns, const std::string& source_peer_id, const std::string& origin_peer_id)
    {
        bool update_peers { false };

        const auto iter = _pub_intent_namespaces.find(ns);
        if (iter == _pub_intent_namespaces.end()) {
            DEBUG("New published intent message origin %s", origin_peer_id.c_str());
            update_peers = true;

            std::map<std::string, std::list<std::string>> intent_map;
            intent_map.try_emplace(origin_peer_id, std::initializer_list<std::string>{ source_peer_id});
            _pub_intent_namespaces.try_emplace(ns, intent_map);

        } else {
            const auto it = iter->second.find(origin_peer_id);
            if (it == iter->second.end()) {
                DEBUG("New published intent origin %s", origin_peer_id.c_str());
                update_peers = true;

                iter->second.try_emplace(origin_peer_id,
                                         std::initializer_list<std::string>{ source_peer_id });
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

    void PeerManager::publishIntentDonePeers(const Namespace& ns, const std::string& source_peer_id, const std::string& origin_peer_id)
    {
        const auto iter = _pub_intent_namespaces.find(ns);
        if (iter != _pub_intent_namespaces.end()) {
            DEBUG("Removing publish intent ns: %s origin %s",
                  ns.to_hex().c_str(),
                  origin_peer_id.c_str());
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
        LOG_INFO("Running peer manager queue receive thread");

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
                        DEBUG("Received subscribe message name: %s", std::string(obj->nspace).c_str());

                        subscribePeers(obj->nspace, obj->source_peer_id);

                        break;
                    }

                    case PeerObjectType::UNSUBSCRIBE: {
                        DEBUG("Received unsubscribe message name: %s", std::string(obj->nspace).c_str());

                        unSubscribePeers(obj->nspace, obj->source_peer_id);
                        break;
                    }

                    case PeerObjectType::PUBLISH_INTENT: {
                        std::string origin_peer_id = obj->origin_peer_id;

                        if (! obj->source_peer_id.compare(CLIENT_PEER_ID)) {
                            origin_peer_id = _config.peer_config.id;
                        }

                        DEBUG("Received publish intent message name: %s origin: %s",
                              std::string(obj->nspace).c_str(), origin_peer_id.c_str());

                        publishIntentPeers(obj->nspace, obj->source_peer_id, origin_peer_id);

                        for (const auto [ns, peer_id]: _peer_sess_subscribed) {
                            subscribePeers(ns, peer_id);
                        }

                        break;
                    }

                    case PeerObjectType::PUBLISH_INTENT_DONE: {
                        std::string origin_peer_id = obj->origin_peer_id;

                        if (! obj->source_peer_id.compare(CLIENT_PEER_ID)) {
                            origin_peer_id = _config.peer_config.id;
                        }

                        DEBUG("Received publish intent done message name: %s origin: %s",
                              std::string(obj->nspace).c_str(), origin_peer_id.c_str());

                        publishIntentDonePeers(obj->nspace, obj->source_peer_id, origin_peer_id);

                        break;
                    }
                }
            }
        }
    }

    /*
     * Delegate Implementations
     */
    void PeerManager::on_connection_status(const TransportContextId& context_id, const TransportStatus status) {

        auto peer_iter = _server_peer_sessions.find(context_id);

        switch (status) {
            case TransportStatus::Ready: {
                // nothing to do for incoming connections
                break;
            }

            case TransportStatus::Disconnected: {
                if (peer_iter == _server_peer_sessions.end())
                    break;

                LOG_INFO("Peer context_id: %" PRIu64 " is disconnected, closing peer connection", context_id);

                _server_peer_sessions.erase(peer_iter);
            }
            default: {
                // ignored
            }
        }
    }

    void PeerManager::on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) {
        auto peer_iter = _server_peer_sessions.find(context_id);

        if (peer_iter == _server_peer_sessions.end()) {
            LOG_INFO("New server accepted peer, context_id: %" PRIu64, context_id);

            TransportRemote peer; // Server doesn't define remotes
            auto [iter, inserted] = _server_peer_sessions.try_emplace(context_id,
                                                                      true,
                                                                      context_id,
                                                                      _config,
                                                                      std::move(peer),
                                                                      _peer_queue,
                                                                      _cache,
                                                                      _subscriptions);

            peer_iter = iter;

            auto& peer_sess = peer_iter->second;

            LOG_INFO("Peer context_id: %" PRIu64 " is ready, creating datagram and control streams", context_id);

            peer_sess.setTransport(_server_transport);
            peer_sess.connect();

            // TODO: On new connection, send publish intents
            for (const auto& [ns, origins]: _pub_intent_namespaces) {
                for (const auto [o, l]: origins) {
                    peer_sess.sendPublishIntent(ns, o);
                }
            }
        }

    }

    void PeerManager::on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) {
        auto peer_iter = _server_peer_sessions.find(context_id);

        if (peer_iter != _server_peer_sessions.end()) {
            peer_iter->second.on_recv_notify(context_id, streamId);
        }
    }



} // namespace laps