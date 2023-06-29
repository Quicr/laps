
#include "peer_manager.h"
#include "peer_session.h"
#include <quicr/quicr_client.h>

#include <sstream>

namespace laps {

    PeerSession::PeerSession(const bool is_inbound,
                             const TransportContextId context_id,
                             const Config& cfg,
                             const TransportRemote& peer_remote,
                             safeQueue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions)
      : peer_config(peer_remote)
      , _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
      , _is_inbound(is_inbound)
      , t_context_id(context_id)
    {
        logger = cfg.logger;

        peer_id = peer_config.host_or_ip;

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

            if (_transport)
                _transport = nullptr;

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

    void PeerSession::publishObject(const messages::PublishDatagram& obj) {

        auto iter = _subscribed.find(obj.header.name);
        if (iter != _subscribed.end()) {
            messages::MessageBuffer mb;
            mb << obj;
            _transport->enqueue(t_context_id, iter->second, mb.take());
        }
    }

    void PeerSession::subscribe(const Namespace& ns)
    {
        // TODO: Implement
    }

    void PeerSession::unsubscribe(const Namespace& ns)
    {
        // TODO: Implement
    }

    /*
     * Delegate Implementations
     */
    void PeerSession::on_connection_status(const TransportContextId& context_id, const TransportStatus status) {

        switch (status) {
            case TransportStatus::Ready: {
                _status = Status::CONNECTED;

                LOG_INFO("Peer context_id %" PRIu64 " is ready", context_id);

                // Send connect message
                std::vector<uint8_t> buf(sizeof(MsgConnect) + peer_id.length());
                MsgConnect c_msg;
                c_msg.longitude = longitude;
                c_msg.latitude = latitude;
                c_msg.id_len = peer_id.length();
                std::memcpy(buf.data(), &c_msg, sizeof(c_msg));
                std::memcpy(buf.data() + peer_id.length(), peer_id.data(), sizeof(peer_id.length()));

                _transport->enqueue(context_id, control_stream_id, std::move(buf));

                break;
            }

            case TransportStatus::Disconnected: {
                _status = Status::DISCONNECTED;

                LOG_INFO("Peer context_id %" PRIu64 " is disconnected", context_id);
            }
        }
    }

    void PeerSession::on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) {
        // Not used for outgoing connections. Incoming connections are handled by the server delegate
    }

    void PeerSession::on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) {
        for (int i = 0; i < 100; i++) {
            auto data = _transport->dequeue(context_id, streamId);

            if (data.has_value()) {
                try {
                    auto msg_type = static_cast<messages::MessageType>(data->front());
                    messages::MessageBuffer msg_buffer{ data.value() };


                    switch (msg_type) {
                        case messages::MessageType::PeerMsg: {
                            auto data = msg_buffer.take();
                            auto subtype = static_cast<PeeringSubType>(data[1]);


                            switch (subtype) {
                                case PeeringSubType::CONNECT: {
                                    MsgConnect c_msg;
                                    std::memcpy(&c_msg, data.data(), sizeof(c_msg));

                                    LOG_INFO("Received peer connect message from");

                                    break;
                                }
                                case PeeringSubType::CONNECT_OK:
                                    break;

                                case PeeringSubType::SUBSCRIBE:
                                    break;

                                case PeeringSubType::UNSUBSCRIBE:
                                    break;

                                default:
                                    LOG_WARN("Unknown subtype");
                                    break;
                            }

                            break;
                        }
                        case messages::MessageType::Publish: {
                            messages::PublishDatagram datagram;
                            msg_buffer >> datagram;

                            if (not _config.disable_dedup &&
                                _cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
                                // duplicate, ignore
                                DEBUG("Duplicate message Name: %s", std::string(datagram.header.name).c_str());
                                return;

                            } else {
                                _cache.put(datagram.header.name, datagram.header.offset_and_fin, datagram.media_data);
                            }

                            // Send to peers
                            _peer_queue.push({ PeerObjectType::PUBLISH, peer_id, datagram });

                            std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list =
                              _subscriptions.find(datagram.header.name);

                            for (const auto& cMgr : list) {
                                for (const auto& dest : cMgr.second) {

                                    dest.second.sendObjFunc(datagram);
                                }

                                break;
                            }
                        }
                        default:
                            LOG_INFO("Invalid Message Type");
                            break;
                    }
                } catch (const messages::MessageBuffer::ReadException& /* ex */) {
                    LOG_ERR("Received read exception error while reading from message buffer.");
                    continue;
                } catch (const std::exception& /* ex */) {
                    LOG_ERR("Received standard exception error while reading from message buffer.");
                    continue;
                } catch (...) {
                    LOG_ERR("Received unknown error while reading from message buffer.");
                    continue;
                }

            } else {
                break;
            }
        }
    }



} // namespace laps