#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "subscription.h"
#include <set>

#include "cache.h"
#include "client_manager.h"

namespace laps {

ClientManager::ClientManager(const Config &cfg, Cache &cache)
    : config(cfg), cache(cache) {

  logger = cfg.logger;

  subscribeList = new Subscriptions(cfg);

  quicr::RelayInfo relayInfo = {.hostname = config.client_bind_addr,
                                .port = config.client_port,
                                .proto = quicr::RelayInfo::Protocol::UDP};

  server =
      std::make_unique<quicr::QuicRServer>(relayInfo, *this, *config.logger);
}

ClientManager::~ClientManager() {
  if (subscribeList != NULL) {
    delete subscribeList;
  }
}

void ClientManager::start() { server->run(); }

void ClientManager::onPublishIntent(const quicr::Namespace & /* quicr_name */,
                                    const std::string & /* origin_url */,
                                    bool /* use_reliable_transport */,
                                    const std::string & /* auth_token */,
                                    quicr::bytes && /* e2e_token */) {}

void ClientManager::onPublisherObject(
    const qtransport::TransportContextId &context_id,
    const qtransport::MediaStreamId &stream_id,
    bool /* use_reliable_transport */,
    quicr::messages::PublishDatagram &&datagram) {

  DEBUG(
      "onPublishedObject Name: %s from context_id: %d stream_id: %d offset: %d",
      datagram.header.name.to_hex().c_str(), context_id, stream_id,
      datagram.header.offset_and_fin);

  /* TODO: Remove duplicate check for now till apps support unique names
if (cache.exists(quicr_name)) {
// duplicate, ignore
DEBUG("Duplicate message Name: %s", quicr_name.to_hex().c_str());
return;

} else {
cache.put(quicr_name, data);
} */
  cache.put(datagram.header.name, datagram.media_data);

  std::list<Subscriptions::Remote> list =
      subscribeList->find(datagram.header.name);

  // TODO: Send to peers (aka other relays)

  for (auto dest : list) {

    if (dest.context_id == context_id && dest.stream_id == stream_id) {
      // split horizon - drop packets back to the source that originated the
      // published object
      DEBUG("Subscriber is source, dropping object '%s' to subscriber %d",
            datagram.header.name.to_hex().c_str(), dest.subscribe_id);
      continue;
    }

    DEBUG("Sending object '%s' to subscriber %d (%d/%d)",
          datagram.header.name.to_hex().c_str(), dest.subscribe_id,
          dest.context_id, dest.stream_id);

    server->sendNamedObject(dest.subscribe_id, false, datagram);
  }
}

void ClientManager::onSubscribe(
    const quicr::Namespace &quicr_namespace, const uint64_t &subscriber_id,
    const qtransport::TransportContextId &context_id,
    const qtransport::MediaStreamId &stream_id,
    const quicr::SubscribeIntent /* subscribe_intent */,
    const std::string & /* origin_url */, bool /* use_reliable_transport */,
    const std::string & /* auth_token */, quicr::bytes && /* data */) {

  DEBUG("onSubscribe namespace: %s/%d", quicr_namespace.to_hex().c_str(),
        quicr_namespace.length());

  Subscriptions::Remote remote = {
      .subscribe_id = subscriber_id,
      .context_id = context_id,
      .stream_id = stream_id,
  };
  subscribeList->add(quicr_namespace.name(), quicr_namespace.length(), remote);

  // respond with response
  auto result = quicr::SubscribeResult{
      quicr::SubscribeResult::SubscribeStatus::Ok, "", {}, {}};
  server->subscribeResponse(subscriber_id, quicr_namespace, result);
}

void ClientManager::onUnsubscribe(const quicr::Namespace& quicr_namespace,
                                  const uint64_t& subscriber_id,
                                  const std::string& /* auth_token */) {

  DEBUG("onUnsubscribe namespace: %s/%d", quicr_namespace.to_hex().c_str(),
        quicr_namespace.length());


  server->subscriptionEnded(subscriber_id, quicr_namespace, quicr::SubscribeResult::SubscribeStatus::Ok);

  subscribeList->remove(quicr_namespace.name(), quicr_namespace.length(), subscriber_id);
}

} // namespace laps