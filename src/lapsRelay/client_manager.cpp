#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "client_subscriptions.h"
#include <set>

#include "cache.h"
#include "client_manager.h"

namespace laps {

ClientManager::ClientManager(const Config& cfg, Cache& cache,
                             ClientSubscriptions& subscriptions)
    : subscribeList(subscriptions)
      , config(cfg), cache(cache)
      , client_mgr_id(cfg.client_port) {

  logger = cfg.logger;

  quicr::RelayInfo relayInfo = {.hostname
                                = config.client_bind_addr,
                                .port = config.client_port,
                                .proto = config.protocol };

  qtransport::TransportConfig tcfg { .tls_cert_filename = config.tls_cert_filename,
                                     .tls_key_filename = config.tls_key_filename,
                                     .time_queue_init_queue_size = config.data_queue_size };

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
        quicr_namespace.to_hex().c_str(), quicr_namespace.length());

  // TODO: Add publish intent state and

  // respond with response
  /* TODO: Add the below
  auto  result = quicr::PublishIntentResult { quicr::messages::Response::Ok,
                                              {}, {} };

  server->publishIntentResponse(subscriber_id, quicr_namespace, result);
 */
}


  void ClientManager::onPublishIntentEnd(
    [[maybe_unused]] const quicr::Namespace& quicr_namespace,
    [[maybe_unused]] const std::string& auth_token,
    [[maybe_unused]] quicr::bytes&& e2e_token) {

  }

void ClientManager::onPublisherObject(
    const qtransport::TransportContextId &context_id,
    [[maybe_unused]] const qtransport::StreamId &stream_id,
    bool /* use_reliable_transport */,
    quicr::messages::PublishDatagram &&datagram) {

  if (not config.disable_dedup &&
      cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
    // duplicate, ignore
    DEBUG("Duplicate message Name: %s", datagram.header.name.to_hex().c_str());
    return;

  } else {
    cache.put(datagram.header.name, datagram.header.offset_and_fin,
              datagram.media_data);
  }

  std::map<uint16_t, std::map<uint64_t, ClientSubscriptions::Remote>> list =
      subscribeList.find(datagram.header.name);

  // TODO: Add sending to peers (aka other relays)

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

  DEBUG("onSubscribe namespace: %s %d (%" PRIu64 "/%" PRIu64 ")",
        quicr_namespace.to_hex().c_str(), quicr_namespace.length(),
        subscriber_id, context_id, stream_id);

  ClientSubscriptions::Remote remote = {
      .client_mgr_id = client_mgr_id,
      .subscribe_id = subscriber_id,
      .conn_id = context_id,
      .sendObjFunc = [&, subscriber_id, context_id, stream_id]
                      (const quicr::messages::PublishDatagram& datagram) {

        server->sendNamedObject(subscriber_id,false, 1,
                                config.time_queue_ttl_default, datagram);
      }
  };
  subscribeList.add(quicr_namespace.name(), quicr_namespace.length(),
                     client_mgr_id, remote);

  // respond with response
  auto result = quicr::SubscribeResult{
      quicr::SubscribeResult::SubscribeStatus::Ok, "", {}, {}};
  server->subscribeResponse(subscriber_id, quicr_namespace, result);
}

void ClientManager::onUnsubscribe(const quicr::Namespace &quicr_namespace,
                                  const uint64_t &subscriber_id,
                                  const std::string & /* auth_token */) {

  DEBUG("onUnsubscribe namespace: %s", quicr_namespace.to_hex().c_str());

  server->subscriptionEnded(subscriber_id, quicr_namespace,
                            quicr::SubscribeResult::SubscribeStatus::Ok);

  subscribeList.remove(quicr_namespace.name(), quicr_namespace.length(),
                        client_mgr_id, subscriber_id);
}


} // namespace laps