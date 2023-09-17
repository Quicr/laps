
#include "peer_manager.h"
#include "peer_session.h"
#include "logger.h"
#include <quicr/quicr_client.h>

#include <sstream>

namespace laps {

    PeerSession::PeerSession(const bool is_inbound,
                             const TransportContextId context_id,
                             const Config& cfg,
                             const TransportRemote& peer_remote,
                             safe_queue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions)
      : peer_config(peer_remote)
      , _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
      , logger(std::make_shared<cantina::Logger>("PEER", cfg.logger))
      , _is_inbound(is_inbound)
      , t_context_id(context_id)
    {
        peer_id = peer_config.host_or_ip;
        _use_reliable = _config.peer_config.use_reliable;

        if (_config.tls_cert_filename.length() == 0) {
            _transport_config.tls_cert_filename = NULL;
            _transport_config.tls_key_filename = NULL;
        }

        FLOG_INFO("Starting peer session");

    }

    PeerSession::~PeerSession()
    {
        if (not _is_inbound) {
            _transport = nullptr;
        }

        FLOG_INFO("Removing peer session with " << peer_config.host_or_ip.c_str() << ":" << peer_config.port
                                                << " id: " << peer_id.c_str());
    }

    PeerSession::Status PeerSession::status() {
        return _status;
    }

    void PeerSession::connect() {
        _status = Status::CONNECTING;

        if (not _is_inbound) {
            if (_transport)
                _transport = nullptr;

            _transport = qtransport::ITransport::make_client_transport(peer_config, _transport_config, *this, logger);
            t_context_id = _transport->start();

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
            //FLOG_DEBUG("Sending published object for name: " << obj.header.name);
            messages::MessageBuffer mb;
            mb << obj;
            _transport->enqueue(t_context_id, iter->second, mb.take());
        }
    }

    void PeerSession::addSubscription(const Namespace& ns)
    {
        if (_use_reliable) {
            auto stream_id = createStream(t_context_id, true);

            _subscribed.try_emplace(ns, stream_id);

        } else {
            _subscribed.try_emplace(ns, dgram_stream_id);
        }
    }

    void PeerSession::sendSubscribe(const Namespace& ns)
    {
        if (_subscribed.count(ns)) // Ignore if ready subscribed
            return;

        addSubscription(ns);

        FLOG_DEBUG("Sending subscribe to peer " << peer_id << " for ns: " <<  ns);

        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });

        std::vector<uint8_t> buf(sizeof(MsgSubscribe) + ns_array.size());

        MsgSubscribe msg;
        msg.count_subscribes = 1;

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), ns_array.data(), ns_array.size());

        _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
    }

    void PeerSession::sendUnsubscribe(const Namespace& ns)
    {
        auto iter = _subscribed.find(ns);
        if (iter != _subscribed.end()) {
            FLOG_DEBUG("Sending unsubscribe to peer " << peer_id << " for ns: " << ns);

            if (_config.peer_config.use_reliable && iter->second)
                _transport->closeStream(t_context_id, iter->second);

            _subscribed.erase(iter);

            std::vector<uint8_t> ns_array;
            encodeNamespaces(ns_array, { ns });

            std::vector<uint8_t> buf(sizeof(MsgUnsubscribe) + ns_array.size());

            MsgUnsubscribe msg;
            msg.count_subscribes = 1;

            std::memcpy(buf.data(), &msg, sizeof(msg));
            std::memcpy(buf.data() + sizeof(msg), ns_array.data(), ns_array.size());

            _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
        }
    }

    void PeerSession::sendConnect()
    {
        std::vector<uint8_t> buf(sizeof(MsgConnect) + _config.peer_config.id.length());
        MsgConnect c_msg;
        c_msg.flag_reliable = _config.peer_config.use_reliable ? 1 : 0;
        c_msg.flag_reserved = 0;
        c_msg.longitude = _config.peer_config.longitude;
        c_msg.latitude = _config.peer_config.latitude;
        c_msg.id_len = _config.peer_config.id.length();
        std::memcpy(buf.data(), &c_msg, sizeof(c_msg));
        std::memcpy(buf.data() + sizeof(c_msg), _config.peer_config.id.data(), _config.peer_config.id.length());

        _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
    }

    void PeerSession::sendConnectOk()
    {
        std::vector<uint8_t> buf(sizeof(MsgConnectOk) + _config.peer_config.id.length());
        MsgConnectOk cok_msg;
        cok_msg.longitude = _config.peer_config.longitude;
        cok_msg.latitude = _config.peer_config.latitude;
        cok_msg.id_len = _config.peer_config.id.length();

        std::memcpy(buf.data(), &cok_msg, sizeof(cok_msg));
        std::memcpy(buf.data() + sizeof(cok_msg),
                    _config.peer_config.id.data(), _config.peer_config.id.length());

        _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
    }

    void PeerSession::sendPublishIntent(const Namespace& ns, const peer_id_t& origin_peer_id)
    {
        FLOG_INFO("Adding to state publish intent " << ns);

        _publish_intents.try_emplace(ns, origin_peer_id);

        // Only send the intents if connected
        if (_status != Status::CONNECTED)
            return;

        FLOG_INFO("Sending publish intent " << ns << " origin: " << origin_peer_id);

        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });


        std::vector<uint8_t> buf(sizeof(MsgPublishIntent) + origin_peer_id.length() + ns_array.size());

        MsgPublishIntent msg;
        msg.origin_id_len = origin_peer_id.length();
        msg.count_publish_intents = 1;

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), origin_peer_id.data(), origin_peer_id.length());
        std::memcpy(buf.data() + sizeof(msg) + msg.origin_id_len,ns_array.data(), ns_array.size());

        _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
    }

    void PeerSession::sendPublishIntentDone(const Namespace& ns, const peer_id_t& origin_peer_id)
    {
        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });

        _publish_intents.erase(ns);

        FLOG_INFO("Sending publish intent DONE " << ns);

        std::vector<uint8_t> buf(sizeof(MsgPublishIntentDone) + origin_peer_id.length() + ns_array.size());

        MsgPublishIntentDone msg;
        msg.origin_id_len = origin_peer_id.length();
        msg.count_publish_intents = 1;

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), origin_peer_id.data(), origin_peer_id.length());
        std::memcpy(buf.data() + sizeof(msg) + msg.origin_id_len,ns_array.data(), ns_array.size());

        _transport->enqueue(t_context_id, control_stream_id, std::move(buf));
    }


    /*
     * Delegate Implementations
     */
    void PeerSession::on_connection_status(const TransportContextId& context_id, const TransportStatus status) {

        switch (status) {
            case TransportStatus::Ready: {
                _status = Status::CONNECTED;

                FLOG_INFO("Peer context_id " << context_id << " is ready, sending connect message");

                sendConnect();

                break;
            }

            case TransportStatus::Disconnected: {
                _status = Status::DISCONNECTED;

                FLOG_INFO("Peer context_id " << context_id << " is disconnected");

                // TODO: cleanup intents and subscriptions
            }
        }
    }

    void PeerSession::on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) {
        // Not used for outgoing connections. Incoming connections are handled by the server delegate
    }

    void PeerSession::on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) {
        for (int i = 0; i < 100; i++) {
            if (auto data = _transport->dequeue(context_id, streamId)) {
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

                                    char c_peer_id[256] { 0 };
                                    std::memcpy(c_peer_id, data.data() + sizeof(c_msg), c_msg.id_len);
                                    peer_id = c_peer_id;

                                    _use_reliable = c_msg.flag_reliable;
                                    longitude = c_msg.longitude;
                                    latitude = c_msg.latitude;

                                    FLOG_INFO("Received peer connect message from "
                                              << peer_id << " reliable: " << _use_reliable << ", sending ok");
                                    sendConnectOk();
                                    _status = Status::CONNECTED;

                                    for (const auto& [ns, o]: _publish_intents) {
                                        if (o.compare(peer_id)) // Send intents not from this peer
                                            sendPublishIntent(ns, o);
                                    }

                                    break;
                                }
                                case PeeringSubType::CONNECT_OK: {
                                    MsgConnectOk cok_msg;
                                    std::memcpy(&cok_msg, data.data(), sizeof(cok_msg));

                                    char c_peer_id[256] {0};
                                    std::memcpy(c_peer_id, data.data() + sizeof(cok_msg), cok_msg.id_len);
                                    peer_id = c_peer_id;

                                    longitude = cok_msg.longitude;
                                    latitude = cok_msg.latitude;

                                    _status = Status::CONNECTED;
                                    FLOG_INFO("Received peer connect OK message from " << peer_id);

                                    // Upon connection, send all publish intents
                                    for (const auto& [ns, o]: _publish_intents) {
                                        if (o.compare(peer_id))
                                            sendPublishIntent(ns, o);
                                    }

                                    break;
                                }

                                case PeeringSubType::SUBSCRIBE: {
                                    MsgSubscribe msg;
                                    std::memcpy(&msg, data.data(), sizeof(msg));

                                    std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg), data.end());
                                    std::vector<Namespace> ns_list;
                                    decodeNamespaces(encoded_ns, ns_list);

                                    FLOG_INFO("Recv subscribe " << ns_list.front());
                                    if (ns_list.size() > 0) {
                                        addSubscription(ns_list.front());
                                        _peer_queue.push({ .type = PeerObjectType::SUBSCRIBE,
                                                           .source_peer_id = peer_id,
                                                           .nspace = ns_list.front() });
                                    }
                                    break;
                                }

                                case PeeringSubType::UNSUBSCRIBE: {
                                    MsgUnsubscribe msg;
                                    std::memcpy(&msg, data.data(), sizeof(msg));

                                    std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg), data.end());
                                    std::vector<Namespace> ns_list;
                                    decodeNamespaces(encoded_ns, ns_list);

                                    FLOG_INFO("Recv unsubscribe " << ns_list.front());

                                    if (ns_list.size() > 0) {
                                        _peer_queue.push({ .type = PeerObjectType::UNSUBSCRIBE,
                                                           .source_peer_id = peer_id,
                                                           .nspace = ns_list.front() });
                                    }
                                    break;
                                }

                                case PeeringSubType::PUBLISH_INTENT: {
                                    MsgPublishIntent msg;
                                    std::memcpy(&msg, data.data(), sizeof(msg));

                                    char o_peer_id[256] { 0 };
                                    std::memcpy(o_peer_id, data.data() + sizeof(msg), msg.origin_id_len);

                                    std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg) + msg.origin_id_len, data.end());
                                    std::vector<Namespace> ns_list;
                                    decodeNamespaces(encoded_ns, ns_list);

                                    if (ns_list.size() > 0) {
                                        FLOG_INFO("Received publish intent from "
                                                  << peer_id << " with namespace: " << ns_list.front());

                                        _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT,
                                                           .source_peer_id = peer_id,
                                                           .origin_peer_id = o_peer_id,
                                                           .nspace = ns_list.front() });
                                    }

                                    break;
                                }

                                case PeeringSubType::PUBLISH_INTENT_DONE: {
                                    MsgPublishIntentDone msg;
                                    std::memcpy(&msg, data.data(), sizeof(msg));

                                    char o_peer_id[256] { 0 };
                                    std::memcpy(o_peer_id, data.data() + sizeof(msg), msg.origin_id_len);

                                    std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg) + msg.origin_id_len, data.end());
                                    std::vector<Namespace> ns_list;
                                    decodeNamespaces(encoded_ns, ns_list);

                                    if (ns_list.size() > 0) {
                                        FLOG_INFO("Received publish intent DONE from "
                                                  << peer_id << " with namespace: " << ns_list.front());

                                        _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT_DONE,
                                                           .source_peer_id = peer_id,
                                                           .origin_peer_id = o_peer_id,
                                                           .nspace = ns_list.front() });
                                    }

                                    break;
                                }

                                default:
                                    FLOG_WARN("Unknown subtype " << static_cast<unsigned>(subtype));
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
                                FLOG_DEBUG("Duplicate message Name: " << datagram.header.name);
                                break;

                            } else {
                                _cache.put(datagram.header.name, datagram.header.offset_and_fin, datagram.media_data);
                            }

                            // Send to peers
                            _peer_queue.push(
                              { .type = PeerObjectType::PUBLISH, .source_peer_id = peer_id, .pub_obj = datagram });

                            std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list =
                              _subscriptions.find(datagram.header.name);

                            for (const auto& cMgr : list) {
                                for (const auto& dest : cMgr.second) {
                                    dest.second.sendObjFunc(datagram);
                                }

                                break;
                            }

                            break;
                        }
                        default:
                            FLOG_INFO("Invalid Message Type " << static_cast<unsigned>(msg_type));
                            break;
                    }
                } catch (const messages::MessageBuffer::ReadException& /* ex */) {
                    FLOG_ERR("Received read exception error while reading from message buffer.");
                    continue;
                } catch (const std::exception& /* ex */) {
                    FLOG_ERR("Received standard exception error while reading from message buffer.");
                    continue;
                } catch (...) {
                    FLOG_ERR("Received unknown error while reading from message buffer.");
                    continue;
                }

            } else {
                break;
            }
        }
    }
} // namespace laps
