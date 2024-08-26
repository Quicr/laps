
#include "peer_manager.h"
#include "peer_session.h"
#include <quicr/quicr_client.h>

#include <sstream>

namespace laps {

    PeerSession::PeerSession(const bool is_inbound,
                             const TransportConnId conn_id,
                             const Config& cfg,
                             const TransportRemote& peer_remote,
                             SafeQueue<PeerObject>& peer_queue,
                             Cache& cache,
                             ClientSubscriptions& subscriptions
#ifndef LIBQUICR_WITHOUT_INFLUXDB
                             , std::shared_ptr<MetricsExporter> mexport
#endif
                             )
      : peer_config(peer_remote)
      , _config(cfg)
      , _peer_queue(peer_queue)
      , _cache(cache)
      , _subscriptions(subscriptions)
      , logger(cfg.logger)
      , _is_inbound(is_inbound)
      , t_conn_id(conn_id)
#ifndef LIBQUICR_WITHOUT_INFLUXDB
      , _mexport(mexport)
#endif
    {
        peer_id = peer_config.host_or_ip;
        _use_reliable = _config.peer_config.use_reliable;

        if (_config.tls_cert_filename.length() == 0) {
            _transport_config.tls_cert_filename = "";
            _transport_config.tls_key_filename = "";
        }

        SPDLOG_LOGGER_INFO(logger, "Starting peer session");

    }

    PeerSession::~PeerSession()
    {
        if (not _is_inbound) {
            _transport = nullptr;
        }

        SPDLOG_LOGGER_INFO(logger, "Removing peer session with {0}:{1} id: {2}", peer_config.host_or_ip, peer_config.port, peer_id);
    }

    PeerSession::Status PeerSession::status() {
        return _status;
    }

    void PeerSession::connect()
    {
        _status = Status::CONNECTING;

        if (_is_inbound) {
            return;
        }
        _subscribed.clear();

        if (_transport)
            _transport = nullptr;

        _transport = qtransport::ITransport::make_client_transport(peer_config, _transport_config, *this, logger);
        t_conn_id = _transport->start(_mexport->metrics_conn_samples, _mexport->metrics_data_samples);

        // Create the control data context
        control_data_ctx_id = _transport->createDataContext(t_conn_id, true, 0, true);

        SPDLOG_LOGGER_INFO(logger, "Control stream ID {0}", control_data_ctx_id);

#ifndef LIBQUICR_WITHOUT_INFLUXDB
        _mexport->set_data_ctx_info(t_conn_id, control_data_ctx_id, {.subscribe = true, .nspace = {}});
#endif

    }

    DataContextId PeerSession::createDataCtx(const TransportConnId conn_id,
                                             const bool use_reliable,
                                             const uint8_t priority,
                                             bool is_bidir) {
        return _transport->createDataContext(conn_id, use_reliable, priority, is_bidir);
    }

    void PeerSession::publishObject(const messages::PublishDatagram& obj, bool reliable) {
        auto iter = _subscribed.find(obj.header.name);

        if (iter != _subscribed.end()) {

            auto min_priority = std::min(obj.header.priority, iter->second.priority);
            if (min_priority != iter->second.priority) {
                iter->second.priority = min_priority;
                _transport->setDataCtxPriority(t_conn_id, iter->second.data_ctx_id, iter->second.priority);
            }

            ITransport::EnqueueFlags eflags { reliable, false, false, false };

            messages::MessageBuffer mb;
            mb << obj;

            // Check if group has incremented, if so start a new stream (reliable using per group)
            if (reliable && iter->second.prev_group_id && iter->second.prev_group_id < obj.header.group_id) {
                eflags.clear_tx_queue = true;
                eflags.new_stream = true;
                eflags.use_reset = true;
            }

            iter->second.prev_group_id = obj.header.group_id;

            _transport->enqueue(t_conn_id,
                                iter->second.data_ctx_id,
                                mb.take(),
                                { MethodTraceItem{} },
                                obj.header.priority,
                                _config.time_queue_ttl_default,
                                0,
                                eflags);
        }
    }

    DataContextId PeerSession:: addSubscription(const Namespace& ns)
    {
        auto data_ctx_id = createDataCtx(t_conn_id, _use_reliable, 2);


#ifndef LIBQUICR_WITHOUT_INFLUXDB
        _mexport->set_data_ctx_info(t_conn_id, data_ctx_id, {.subscribe = true, .nspace = ns});
#endif

        auto ctx = _subscribed.try_emplace(ns, SubscribeContext{ data_ctx_id });

        if (!ctx.second) {
            ctx.first->second.data_ctx_id = data_ctx_id;
        }

        return data_ctx_id;
    }



    void PeerSession::removeSubscription(const Namespace& ns) {
        auto iter = _subscribed.find(ns);
        if (iter != _subscribed.end()) {
            SPDLOG_LOGGER_DEBUG(logger, "Removing subscription {0} from peer {1}", std::string(ns), peer_id);

            _transport->deleteDataContext(t_conn_id, iter->second.data_ctx_id);
            _subscribed.erase(iter);
        }
    }

    void PeerSession::sendSubscribe(const Namespace& ns)
    {
        if (_subscribed.count(ns)) // Ignore if ready subscribed
            return;

        const auto data_ctx_id = addSubscription(ns);

        SPDLOG_LOGGER_DEBUG(logger, "Sending subscribe to peer {0} for ns: {1}", peer_id, std::string(ns));

        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });

        std::vector<uint8_t> buf(sizeof(MsgSubscribe) + ns_array.size());

        MsgSubscribe msg;
        msg.remote_data_ctx_id = data_ctx_id;
        msg.count_subscribes = 1;
        msg.msg_len = sizeof(msg) + ns_array.size();

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), ns_array.data(), ns_array.size());

        _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));
    }

    void PeerSession::sendUnsubscribe(const Namespace& ns)
    {
        auto iter = _subscribed.find(ns);
        if (iter != _subscribed.end()) {
            SPDLOG_LOGGER_DEBUG(logger, "Sending unsubscribe to peer {0} for ns: {1}", peer_id, std::string(ns));

            std::vector<uint8_t> ns_array;
            encodeNamespaces(ns_array, { ns });

            std::vector<uint8_t> buf(sizeof(MsgUnsubscribe) + ns_array.size());

            MsgUnsubscribe msg;
            msg.count_subscribes = 1;
            msg.msg_len = sizeof(msg) + ns_array.size();

            std::memcpy(buf.data(), &msg, sizeof(msg));
            std::memcpy(buf.data() + sizeof(msg), ns_array.data(), ns_array.size());

            _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));

            removeSubscription(ns);
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

        c_msg.msg_len = sizeof(c_msg) + _config.peer_config.id.length();

        std::memcpy(buf.data(), &c_msg, sizeof(c_msg));
        std::memcpy(buf.data() + sizeof(c_msg), _config.peer_config.id.data(), _config.peer_config.id.length());

        _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));
    }

    void PeerSession::sendConnectOk()
    {
        std::vector<uint8_t> buf(sizeof(MsgConnectOk) + _config.peer_config.id.length());
        MsgConnectOk cok_msg;
        cok_msg.longitude = _config.peer_config.longitude;
        cok_msg.latitude = _config.peer_config.latitude;
        cok_msg.id_len = _config.peer_config.id.length();

        cok_msg.msg_len = sizeof(cok_msg) + _config.peer_config.id.length();

        std::memcpy(buf.data(), &cok_msg, sizeof(cok_msg));
        std::memcpy(buf.data() + sizeof(cok_msg),
                    _config.peer_config.id.data(), _config.peer_config.id.length());

        _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));
    }

    void PeerSession::sendPublishIntent(const Namespace& ns, const peer_id_t& origin_peer_id)
    {
        SPDLOG_LOGGER_INFO(logger, "Adding to state publish intent {0}", std::string(ns));

        _publish_intents.try_emplace(ns, origin_peer_id);

        // Only send the intents if connected
        if (_status != Status::CONNECTED)
            return;

        SPDLOG_LOGGER_INFO(logger, "Sending publish intent {0} origin: {1} peer: {2}", std::string(ns), origin_peer_id, peer_id);

        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });


        std::vector<uint8_t> buf(sizeof(MsgPublishIntent) + origin_peer_id.length() + ns_array.size());

        MsgPublishIntent msg;
        msg.origin_id_len = origin_peer_id.length();
        msg.count_publish_intents = 1;
        msg.msg_len = sizeof(msg) + origin_peer_id.length() + ns_array.size();

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), origin_peer_id.data(), origin_peer_id.length());
        std::memcpy(buf.data() + sizeof(msg) + msg.origin_id_len,ns_array.data(), ns_array.size());

        _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));
    }

    void PeerSession::sendPublishIntentDone(const Namespace& ns, const peer_id_t& origin_peer_id)
    {
        std::vector<uint8_t> ns_array;
        encodeNamespaces(ns_array, { ns });

        _publish_intents.erase(ns);

        SPDLOG_LOGGER_INFO(logger, "Sending publish intent DONE to peer: {0} ns: {1}", peer_id, std::string(ns));

        std::vector<uint8_t> buf(sizeof(MsgPublishIntentDone) + origin_peer_id.length() + ns_array.size());

        MsgPublishIntentDone msg;
        msg.origin_id_len = origin_peer_id.length();
        msg.count_publish_intents = 1;
        msg.msg_len = sizeof(msg) + origin_peer_id.length() + ns_array.size();

        std::memcpy(buf.data(), &msg, sizeof(msg));
        std::memcpy(buf.data() + sizeof(msg), origin_peer_id.data(), origin_peer_id.length());
        std::memcpy(buf.data() + sizeof(msg) + msg.origin_id_len,ns_array.data(), ns_array.size());

        _transport->enqueue(t_conn_id, control_data_ctx_id, std::move(buf));
    }


    /*
     * Delegate Implementations
     */
    void PeerSession::on_connection_status(const TransportConnId& conn_id, const TransportStatus status) {

        switch (status) {
            case TransportStatus::Ready: {
                _status = Status::CONNECTED;

                SPDLOG_LOGGER_INFO(logger, "Peer conn_id {0} is ready, sending connect message", conn_id);

                sendConnect();

                break;
            }

            case TransportStatus::Disconnected: {
                _status = Status::DISCONNECTED;

                SPDLOG_LOGGER_INFO(logger, "Peer conn_id {0} is disconnected", conn_id);

                // No need to close data contexts when connection is closed/disconnected
                _subscribed.clear();
                break;
            }

            case TransportStatus::Connecting:
                break;
            case TransportStatus::RemoteRequestClose:
                break;
            case TransportStatus::Shutdown:
                break;
        }
    }

    void PeerSession::on_new_connection(const TransportConnId& conn_id, const TransportRemote& remote) {
        // Not used for outgoing connections. Incoming connections are handled by the server delegate
    }

    void PeerSession::on_recv_stream(const TransportConnId& conn_id,
                                     uint64_t stream_id,
                                     std::optional<DataContextId> data_ctx_id,
                                     const bool is_bidir)
    {
        auto stream_buf = _transport->getStreamBuffer(conn_id, stream_id);

        if (stream_buf == nullptr) {
            return;
        }

        while (true) {
            if (stream_buf->available(4)) {
                auto msg_len_b = stream_buf->front(4);

                if (msg_len_b.empty())
                    return;

                auto* msg_len = reinterpret_cast<uint32_t*>(msg_len_b.data());

                if (!*msg_len) {
                    // Invalid to have message length of zero
                    return;
                }

                if (stream_buf->available(*msg_len)) {
                    auto obj = stream_buf->front(*msg_len);
                    stream_buf->pop(*msg_len);

                    try {
                        messages::MessageBuffer msg_buffer{ obj };

                        handle(stream_id, data_ctx_id, true, std::move(msg_buffer), is_bidir);

                    } catch (const messages::MessageBuffer::ReadException& ex) {

                        // TODO(trigaux): When reliable, we really should reset the stream if
                        // this happens (at least more than once)
                        SPDLOG_LOGGER_CRITICAL(logger, "Received read exception error while reading from message buffer: {0}", ex.what());
                        return;

                    } catch (const std::exception& e) {
                        SPDLOG_LOGGER_CRITICAL(logger,
                                               "Received standard exception error while reading from message buffer: {0}", e.what());
                        return;
                    } catch (...) {
                        SPDLOG_LOGGER_CRITICAL(logger, "Received unknown error while reading from message buffer");
                        return;
                    }
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    void PeerSession::on_recv_dgram(const TransportConnId& conn_id, std::optional<DataContextId> data_ctx_id)
    {
        for (int i = 0; i < 150; i++) {
            auto data = _transport->dequeue(conn_id, data_ctx_id);

            if (!data) {
                return;
            }

            messages::MessageBuffer msg_buffer{ *data };

            try {
                handle(std::nullopt, std::nullopt, false, std::move(msg_buffer));
            } catch (const std::exception& e) {
                SPDLOG_LOGGER_DEBUG(logger, "Dropping malformed message: {0}", e.what());
                return;
            } catch (...) {
                SPDLOG_LOGGER_CRITICAL(logger, "Received malformed message with unknown fatal error");
                throw;
            }
        }
    }

    void PeerSession::handle(std::optional<uint64_t> stream_id,
                             std::optional<DataContextId> data_ctx_id,
                             bool reliable,
                             messages::MessageBuffer&& msg,
                             [[maybe_unused]] bool is_bidir)
    {
        if (msg.empty()) {
            SPDLOG_LOGGER_ERROR(logger, "stream_id: {0} msg size: {1} is empty", *stream_id, msg.size());
            return;
        }

        auto chdr = msg.front(5); // msg_len + type
        auto msg_type = static_cast<messages::MessageType>(chdr.back());

        switch (msg_type) {
            case messages::MessageType::PeerMsg: {
                auto data = msg.take();
                auto subtype = static_cast<PeeringSubType>(data[5]);

                switch (subtype) {
                    case PeeringSubType::CONNECT: {
                        MsgConnect c_msg;
                        std::memcpy(&c_msg, data.data(), sizeof(c_msg));

                        char c_peer_id[256]{ 0 };
                        std::memcpy(c_peer_id, data.data() + sizeof(c_msg), c_msg.id_len);
                        peer_id = c_peer_id;

                        _use_reliable = c_msg.flag_reliable;
                        longitude = c_msg.longitude;
                        latitude = c_msg.latitude;

                        // Only create control context on connnect
                        if (data_ctx_id && is_bidir) {
                            control_data_ctx_id = *data_ctx_id;
                        } else {
                            SPDLOG_LOGGER_ERROR(logger, " conn_id: {0} connect is invalid due to control message not sent over bidir stream", t_conn_id);
                            return;
                        }

                        SPDLOG_LOGGER_INFO(logger,
                                           "Received peer connect message from {0} reliable: {1}, sending ok "
                                           "control_data_ctx: {2} control_stream_id: {3}",
                                           peer_id,
                                           _use_reliable,
                                           control_data_ctx_id,
                                           *stream_id);

#ifndef LIBQUICR_WITHOUT_INFLUXDB
                        _mexport->set_conn_ctx_info(
                          t_conn_id,
                          { .endpoint_id = peer_id, .relay_id = _config.peer_config.id, .data_ctx_info = {} },
                          false);
                        _mexport->set_data_ctx_info(
                          t_conn_id, control_data_ctx_id, { .subscribe = true, .nspace = {} });
#endif
                        sendConnectOk();
                        _status = Status::CONNECTED;

                        for (const auto& [ns, o] : _publish_intents) {
                            if (o.compare(peer_id)) // Send intents not from this peer
                                sendPublishIntent(ns, o);
                        }

                        break;
                    }
                    case PeeringSubType::CONNECT_OK: {
                        MsgConnectOk cok_msg;
                        std::memcpy(&cok_msg, data.data(), sizeof(cok_msg));

                        char c_peer_id[256]{ 0 };
                        std::memcpy(c_peer_id, data.data() + sizeof(cok_msg), cok_msg.id_len);
                        peer_id = c_peer_id;

                        longitude = cok_msg.longitude;
                        latitude = cok_msg.latitude;

                        if (!stream_id || !data_ctx_id) {
                            SPDLOG_LOGGER_WARN(logger, "conn_id: {0} missing stream_id or data_ctx_id for control messages", t_conn_id);
                            return;
                        }
                        _status = Status::CONNECTED;
                        SPDLOG_LOGGER_INFO(logger, "Received peer connect OK message from {0}", peer_id);

#ifndef LIBQUICR_WITHOUT_INFLUXDB
                        _mexport->set_conn_ctx_info(
                          t_conn_id,
                          { .endpoint_id = _config.peer_config.id, .relay_id = peer_id, .data_ctx_info = {} },
                          true);
#endif

                        // Upon connection, send all publish intents
                        for (const auto& [ns, o] : _publish_intents) {
                            if (o.compare(peer_id))
                                sendPublishIntent(ns, o);
                        }

                        break;
                    }

                    case PeeringSubType::SUBSCRIBE: {
                        if (!stream_id || !data_ctx_id) {
                            SPDLOG_LOGGER_WARN(logger, "conn_id: {0} missing stream_id or data_ctx_id for control messages", t_conn_id);
                            return;
                        }

                        MsgSubscribe msg;
                        std::memcpy(&msg, data.data(), sizeof(msg));

                        std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg), data.end());
                        std::vector<Namespace> ns_list;
                        decodeNamespaces(encoded_ns, ns_list);

                        SPDLOG_LOGGER_INFO(logger, "Received subscribe from {0} ns: {1}", peer_id, std::string(ns_list.front()));
                        if (ns_list.size() > 0) {
                            addSubscription(ns_list.front());
                            _peer_queue.push({ .type = PeerObjectType::SUBSCRIBE,
                                                      .source_peer_id = peer_id,
                                                      .nspace = ns_list.front() });
                        }
                        break;
                    }

                    case PeeringSubType::UNSUBSCRIBE: {
                        if (!stream_id || !data_ctx_id) {
                            SPDLOG_LOGGER_WARN(logger, "conn_id: {0} missing stream_id or data_ctx_id for control messages", t_conn_id);
                            return;
                        }

                        MsgUnsubscribe msg;
                        std::memcpy(&msg, data.data(), sizeof(msg));

                        std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg), data.end());
                        std::vector<Namespace> ns_list;
                        decodeNamespaces(encoded_ns, ns_list);

                        SPDLOG_LOGGER_INFO(logger, "Received unsubscribe from {0} ns: {1}", peer_id, std::string(ns_list.front()));

                        if (ns_list.size() > 0) {
                            removeSubscription(ns_list.front());

                            _peer_queue.push({ .type = PeerObjectType::UNSUBSCRIBE,
                                               .source_peer_id = peer_id,
                                               .nspace = ns_list.front() });
                        }
                        break;
                    }

                    case PeeringSubType::PUBLISH_INTENT: {
                        if (!stream_id || !data_ctx_id) {
                            SPDLOG_LOGGER_WARN(logger, "conn_id: {0} missing stream_id or data_ctx_id for control messages", t_conn_id);
                            return;
                        }

                        MsgPublishIntent msg;
                        std::memcpy(&msg, data.data(), sizeof(msg));

                        char o_peer_id[256]{ 0 };
                        std::memcpy(o_peer_id, data.data() + sizeof(msg), msg.origin_id_len);

                        std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg) + msg.origin_id_len, data.end());
                        std::vector<Namespace> ns_list;
                        decodeNamespaces(encoded_ns, ns_list);

                        if (ns_list.size() > 0) {
                            SPDLOG_LOGGER_INFO(logger,
                                               "Received publish intent from {0} origin_peer: {1} with namespace: {2}",
                                               peer_id,
                                               o_peer_id,
                                               std::string(ns_list.front()));

                            _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT,
                                                      .source_peer_id = peer_id,
                                                      .origin_peer_id = o_peer_id,
                                                      .nspace = ns_list.front() });
                        }

                        break;
                    }

                    case PeeringSubType::PUBLISH_INTENT_DONE: {
                        if (!stream_id || !data_ctx_id) {
                            SPDLOG_LOGGER_WARN(logger, "conn_id: {0} missing stream_id or data_ctx_id for control messages", t_conn_id);
                            return;
                        }

                        MsgPublishIntentDone msg;
                        std::memcpy(&msg, data.data(), sizeof(msg));

                        char o_peer_id[256]{ 0 };
                        std::memcpy(o_peer_id, data.data() + sizeof(msg), msg.origin_id_len);

                        std::vector<uint8_t> encoded_ns(data.begin() + sizeof(msg) + msg.origin_id_len, data.end());
                        std::vector<Namespace> ns_list;
                        decodeNamespaces(encoded_ns, ns_list);

                        // TODO: Remove subscription to allow a more optimized path based on new publish intent

                        if (ns_list.size() > 0) {
                            SPDLOG_LOGGER_INFO(logger,
                                               "Received publish intent DONE from {0} with namespace: {1}",
                                               peer_id,
                                               std::string(ns_list.front()));

                            _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT_DONE,
                                               .source_peer_id = peer_id,
                                               .origin_peer_id = o_peer_id,
                                               .nspace = ns_list.front() });
                        }

                        break;
                    }

                    default:
                        SPDLOG_LOGGER_WARN(logger, "Unknown subtype {0}", static_cast<unsigned>(subtype));
                        break;
                }

                break;
            }
            case messages::MessageType::Publish: {
                messages::PublishDatagram datagram;
                msg >> datagram;

                if (not _config.disable_dedup && _cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
                    // duplicate, ignore
                    SPDLOG_LOGGER_DEBUG(logger, "Duplicate message Name: {0}", std::string(datagram.header.name));
                    break;

                } else {
                    _cache.put(datagram.header.name, datagram.header.offset_and_fin, datagram.media_data);
                }

                // Send to peers
                _peer_queue.push({ .type = PeerObjectType::PUBLISH, .source_peer_id = peer_id,
                                          .reliable = reliable,
                                          .pub_obj = datagram });

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
                SPDLOG_LOGGER_INFO(logger, "Invalid Message Type {0}", static_cast<unsigned>(msg_type));
                break;
        }
    }
}
