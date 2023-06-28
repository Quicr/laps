
#include "peer_manager.h"
#include <quicr/quicr_client.h>
#include <sstream>

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

        _client_rx_msg_thr = std::thread(&PeerManager::ClientRxThread, this);

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
                                    cfg.peer_config.listen_port,
                                    cfg.peer_config.protocol == RelayInfo::Protocol::UDP ? TransportProtocol::UDP
                                                                                         : TransportProtocol::QUIC };

            createPeerSession(remote, cfg.peer_config.sub_namespaces);
        }
    }

    PeerManager::~PeerManager()
    {
        // Stop threads
        _stop = true;

        LOG_INFO("Closing peer manager threads");

        // Clear threads from the queue
        _peer_queue.stopWaiting();

        if (_client_rx_msg_thr.joinable())
            _client_rx_msg_thr.join();

        LOG_INFO("Closed peer manager stopped");

        // TODO: Might need to gracefully close peer sessions
    }

    void PeerManager::watchThread(int interval_ms) {

        LOG_INFO("Running peer manager outbound peer connection thread");

        while (not _stop) {
           for (auto& sess : _client_peer_sessions) {
                if (sess.status() == PeerSession::Status::DISCONNECTED) {
                    LOG_INFO("Peer session to %s disconnected, reconnecting", sess.peer_config.host_or_ip.c_str());

                    sess.connect();
                }
           }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }

    }

    void PeerManager::createPeerSession(const TransportRemote &peer_config, const nameList& sub_ns)
    {
        auto &peer_sess = _client_peer_sessions.emplace_back(false, 0,
                                                             _config,
                                                             peer_config,
                                                             _peer_queue,
                                                             _cache,
                                                             _subscriptions,
                                                             _peer_subscriptions);

        peer_sess.connect();
    }

    void PeerManager::ClientRxThread()
    {
        LOG_INFO("Running peer manager client receive thread");

        while (not _stop) {
            const auto& obj = _peer_queue.block_pop();

            if (obj) {
                LOG_INFO("Received publish message name: %s", std::string(obj->pub_obj.header.name).c_str());
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
                                                                      _subscriptions,
                                                                      _peer_subscriptions);

            peer_iter = iter;

            auto& peer_sess = peer_iter->second;

            LOG_INFO("Peer context_id: %" PRIu64 " is ready, creating datagram and control streams");

            peer_sess.setTransport(_server_transport);
            peer_sess.connect();
        }

    }

    void PeerManager::on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) {

    }



} // namespace laps