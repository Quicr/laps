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
    : subscribeList(subscriptions)
      , config(cfg), cache(cache)
      , client_mgr_id(cfg.client_config.listen_port)
      , _peer_queue(peer_queue)
{

  logger = cfg.logger;

  quicr::RelayInfo relayInfo = {.hostname
                                = config.client_config.bind_addr,
                                .port = config.client_config.listen_port,
                                .proto = config.client_config.protocol };

  qtransport::TransportConfig tcfg { .tls_cert_filename = config.tls_cert_filename.empty() ? NULL : config.tls_cert_filename.c_str(),
                                     .tls_key_filename = config.tls_cert_filename.empty() ? NULL : config.tls_key_filename.c_str(),
                                     .time_queue_init_queue_size = config.data_queue_size,
                                     .debug = false };

  logger->log(qtransport::LogLevel::info, "Starting client manager id " + std::to_string(client_mgr_id));

  server =
      std::make_unique<quicr::QuicRServer>(relayInfo, std::move(tcfg), *this, *config.logger);
}

ClientManager::~ClientManager() {
  server.reset();
}

bool ClientManager::ready() {
  if (not running || not server->is_transport_ready())
    return false;
  else
    return true;
}

void ClientManager::start() {
  running = server->run();
}



void ClientManager::onPublishIntent(const quicr::Namespace& quicr_namespace,
                                    const std::string& /* origin_url */,
                                    bool /* use_reliable_transport */,
                                    const std::string& /* auth_token */,
                                    quicr::bytes&& /* e2e_token */) {

  DEBUG("onPublishIntent namespace: %s",
        std::string(quicr_namespace).c_str(), quicr_namespace.length());

  _peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });

  /* TODO: Add the below - Need to direct the response to the correct subscriber
  auto  result = quicr::PublishIntentResult { quicr::messages::Response::Ok,
                                              {}, {} };

  server->publishIntentResponse(subscriber_id, quicr_namespace, result);
 */
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
    const qtransport::TransportContextId &context_id,
    [[maybe_unused]] const qtransport::StreamId &stream_id,
    bool /* use_reliable_transport */,
    quicr::messages::PublishDatagram &&datagram) {

  if (not config.disable_dedup &&
      cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
    // duplicate, ignore
    DEBUG("Duplicate message Name: %s", std::string(datagram.header.name).c_str());
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
          dest.second.conn_id == context_id) {
        continue;
      }

      dest.second.sendObjFunc(datagram);
    }
  }
}

void ClientManager::onSubscribe(
    const quicr::Namespace &quicr_namespace, const uint64_t &subscriber_id,
    const qtransport::TransportContextId &context_id,
    const qtransport::StreamId &stream_id,
    const quicr::SubscribeIntent /* subscribe_intent */,
    const std::string & /* origin_url */, bool /* use_reliable_transport */,
    const std::string & /* auth_token */, quicr::bytes && /* data */) {

  const auto& existing_remote =
    subscribeList.getSubscribeRemote(quicr_namespace, client_mgr_id, subscriber_id);

  if (existing_remote.client_mgr_id != 0) {
    DEBUG("duplicate onSubscribe namespace: %s %d (%" PRIu64 ")",
          std::string(quicr_namespace).c_str(),
          subscriber_id,
          context_id,
          stream_id);
    return;
  }

  DEBUG("onSubscribe namespace: %s %d (%" PRIu64 "/%" PRIu64 ")",
        std::string(quicr_namespace).c_str(), quicr_namespace.length(),
        subscriber_id, context_id, stream_id);

  ClientSubscriptions::Remote remote = {
      .client_mgr_id = client_mgr_id,
      .subscribe_id = subscriber_id,
      .conn_id = context_id,
      .sendObjFunc = [&, subscriber_id]
                      (const quicr::messages::PublishDatagram& datagram) {

        server->sendNamedObject(subscriber_id,false, 1,
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

  DEBUG("onUnsubscribe namespace: %s", std::string(quicr_namespace).c_str());

  server->subscriptionEnded(subscriber_id, quicr_namespace,
                            quicr::SubscribeResult::SubscribeStatus::Ok);

  subscribeList.remove(quicr_namespace.name(), quicr_namespace.length(),
                        client_mgr_id, subscriber_id);

  _peer_queue.push({ .type = PeerObjectType::UNSUBSCRIBE, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });
}


} // namespace laps