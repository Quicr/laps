
#include "peer_manager.h"
#include <quicr/quicr_client.h>
#include <sstream>

namespace laps {

    PeerSession::PeerSession(const bool is_inbound,
                             const TransportContextId context_id,
                             const Config& cfg,
                             const TransportRemote& peer_remote,
                             safeQueue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions,
                             PeerSubscriptions& peer_subscriptions)
      : peer_config(peer_remote)
      , _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
      , _peer_subscriptions(peer_subscriptions)
      , _is_inbound(is_inbound)
      , t_context_id(context_id)
    {
        logger = cfg.logger;

        if (_config.tls_cert_filename.length() == 0) {
            _transport_config.tls_cert_filename = NULL;
            _transport_config.tls_key_filename = NULL;
        }

        LOG_INFO("Starting peer session");

    }

    PeerSession::~PeerSession()
    {
        if (not _is_inbound) {
            _transport = nullptr;
        }

        LOG_INFO("Removing peer session with %s:%d, id: %s ", peer_config.host_or_ip.c_str(),
                 peer_config.port, peer_id.c_str());

    }

    PeerSession::Status PeerSession::status() {
        return _status;
    }

    void PeerSession::connect() {

        if (not _is_inbound) {
            _status = Status::CONNECTING;

            _transport = qtransport::ITransport::make_client_transport(peer_config, _transport_config, *this, *logger);
            t_context_id = _transport->start();

        } else {

            // Inbound is already connected
            _status = Status::CONNECTED;
        }

        // Create the datagram and control streams
        dgram_stream_id = createStream(t_context_id, false);
        control_stream_id = createStream(t_context_id, true);
    }

    StreamId PeerSession::createStream(TransportContextId context_id, bool use_reliable) {
        return _transport->createStream(context_id, use_reliable);
    }

    /*
     * Delegate Implementations
     */
    void PeerSession::on_connection_status(const TransportContextId& context_id, const TransportStatus status) {

        switch (status) {
            case TransportStatus::Ready: {
                _status = Status::CONNECTED;

                LOG_INFO("Peer context_id %" PRIu64 " is ready", context_id);

                break;
            }

            case TransportStatus::Disconnected: {
                _status = Status::DISCONNECTED;

                if (not _is_inbound) {
                    _transport = nullptr;
                }

                LOG_INFO("Peer context_id %" PRIu64 " is disconnected", context_id);
            }
        }
    }

    void PeerSession::on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) {

    }

    void PeerSession::on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) {

    }



} // namespace laps