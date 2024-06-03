#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "client_subscriptions.h"
#include <set>

#include "cache.h"
#include "client_manager.h"

namespace laps {

ClientManager::ClientManager(const Config& cfg, Cache& cache,
                             ClientSubscriptions& subscriptions,
                             peerQueue &peer_queue)
    : logger(std::make_shared<cantina::Logger>("CMGR", cfg.logger))
      , subscribeList(subscriptions)
      , config(cfg), cache(cache)
      , client_mgr_id(cfg.client_config.listen_port)
      , _peer_queue(peer_queue) {}

ClientManager::~ClientManager() {
    logger->info << "Client manager " << client_mgr_id << " ended" << std::flush;
    running = false;
}

bool ClientManager::ready() {
  if (not running || not server->is_transport_ready())
    return false;
  else
    return true;
}

void ClientManager::stop() {
    server.reset();
    running = false;
}

void ClientManager::start() {
  quicr::RelayInfo relayInfo = {.hostname
                                 = config.client_config.bind_addr,
                                 .port = config.client_config.listen_port,
                                 .proto = config.client_config.protocol,
                                 .relay_id = config.peer_config.id };

  qtransport::TransportConfig tcfg { .tls_cert_filename = config.tls_cert_filename.empty() ? NULL : config.tls_cert_filename.c_str(),
                                    .tls_key_filename = config.tls_cert_filename.empty() ? NULL : config.tls_key_filename.c_str(),
                                    .time_queue_init_queue_size = config.data_queue_size,
                                    .time_queue_max_duration = 5000,
                                    .time_queue_rx_size = config.rx_queue_size,
                                    .debug = config.debug,
                                    .quic_cwin_minimum = static_cast<uint64_t>(config.cwin_min_kb * 1024),
                                    .use_reset_wait_strategy = config.use_reset_wait_strategy,
                                    .use_bbr = config.use_bbr,
                                    .quic_qlog_path = config.qlog_path.size() ? config.qlog_path.c_str() : nullptr,
                                    .quic_priority_limit = config.priority_limit_bypass};

  logger->info << "Starting client manager id " << std::to_string(client_mgr_id) << std::flush;

  server =
    std::make_unique<quicr::Server>(relayInfo, std::move(tcfg), get_shared_ptr(), logger);
  running = server->run();
}



void ClientManager::onPublishIntent(const quicr::Namespace& quicr_namespace,
                                    const std::string& /* origin_url */,
                                    const std::string& /* auth_token */,
                                    quicr::bytes&& /* e2e_token */) {

  FLOG_INFO("onPublishIntent namespace: " << quicr_namespace);

  _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });

  auto  result = quicr::PublishIntentResult { quicr::messages::Response::Ok,
                                              {}, {} };

  server->publishIntentResponse(quicr_namespace, result);
}


  void ClientManager::onPublishIntentEnd(
    const quicr::Namespace& quicr_namespace,
    [[maybe_unused]] const std::string& auth_token,
    [[maybe_unused]] quicr::bytes&& e2e_token) {

  _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT_DONE,
                     .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });
  }

void ClientManager::onPublisherObject(
    const qtransport::TransportConnId& conn_id,
    [[maybe_unused]] const qtransport::DataContextId& data_ctx_id,
    quicr::messages::PublishDatagram &&datagram) {

  if (not config.disable_dedup &&
      cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
    // duplicate, ignore
    FLOG_DEBUG("Duplicate message Name: " << datagram.header.name);
    return;

  } else {
    cache.put(datagram.header.name, datagram.header.offset_and_fin,
              datagram.media_data);
  }

  // Send to peers
  _peer_queue.push({ .type =PeerObjectType::PUBLISH,.source_peer_id = CLIENT_PEER_ID, .pub_obj = datagram });

  std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list =
      subscribeList.find(datagram.header.name);

  for (const auto& cMgr: list) {
    for (const auto& dest : cMgr.second) {

      if (not config.disable_splithz &&
          dest.second.client_mgr_id == client_mgr_id &&
          dest.second.conn_id == conn_id) {
        continue;
      }

      dest.second.sendObjFunc(datagram);
    }
  }
}

void ClientManager::onSubscribePause(const quicr::Namespace& quicr_namespace,
                      const uint64_t subscriber_id,
                      const qtransport::TransportConnId conn_id,
                      const qtransport::DataContextId data_ctx_id,
                      const bool pause) {

  FLOG_INFO("onSubscribe " << (pause ? "Pause" : "Resume")
                           << " namespace: " << quicr_namespace
                           << " subscriber_id: " << subscriber_id
                           << " conn_id " << conn_id
                           << " data_ctx_id: " << data_ctx_id);
}

void ClientManager::onSubscribe(
    const quicr::Namespace &quicr_namespace, const uint64_t& subscriber_id,
    const qtransport::TransportConnId& conn_id,
    const qtransport::DataContextId& data_ctx_id,
    const quicr::SubscribeIntent /* subscribe_intent */,
    const std::string & /* origin_url */,
    const std::string & /* auth_token */, quicr::bytes && /* data */) {

  const auto& existing_remote =
    subscribeList.getSubscribeRemote(quicr_namespace, client_mgr_id, subscriber_id);

  if (existing_remote.client_mgr_id != 0) {
    // ignore duplicate
    return;
  }

  FLOG_INFO("onSubscribe namespace: " << quicr_namespace << " subscriber_id: " << subscriber_id << " conn_id "
                                      << conn_id << " data_ctx_id: " << data_ctx_id);

  ClientSubscriptions::Remote remote = {
      .client_mgr_id = client_mgr_id,
      .subscribe_id = subscriber_id,
      .conn_id = conn_id,
      .sendObjFunc = [&, subscriber_id]
                      (const quicr::messages::PublishDatagram& datagram) {

        server->sendNamedObject(subscriber_id, datagram.header.priority,
                                config.time_queue_ttl_default, datagram);
      }
  };
  subscribeList.add(quicr_namespace.name(), quicr_namespace.length(),
                     client_mgr_id, remote);

  _peer_queue.push({ .type = PeerObjectType::SUBSCRIBE, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });


  // respond with response
  auto result = quicr::SubscribeResult{
      quicr::SubscribeResult::SubscribeStatus::Ok, "", {}, {}};
  server->subscribeResponse(subscriber_id, quicr_namespace, result);
}

void ClientManager::onUnsubscribe(const quicr::Namespace &quicr_namespace,
                                  const uint64_t &subscriber_id,
                                  const std::string & /* auth_token */) {

  FLOG_INFO("onUnsubscribe namespace: " << quicr_namespace << " subscriber_id: " << subscriber_id);

  server->subscriptionEnded(subscriber_id, quicr_namespace,
                            quicr::SubscribeResult::SubscribeStatus::Ok);


  subscribeList.remove(quicr_namespace.name(), quicr_namespace.length(),
                        client_mgr_id, subscriber_id);

  _peer_queue.push({ .type = PeerObjectType::UNSUBSCRIBE, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });
}

} // namespace laps
