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

void ClientManager::onPublishIntent(const quicr::Namespace &quicr_name,
                                    const std::string &origin_url,
                                    bool use_reliable_transport,
                                    const std::string &auth_token,
                                    quicr::bytes &&e2e_token) {}

void ClientManager::onPublisherObject(const quicr::Name &quicr_name,
                                      uint8_t priority, uint16_t expiry_age_ms,
                                      bool use_reliable_transport,
                                      quicr::bytes &&data) {

  DEBUG("onPublishedObject Name: %s", quicr_name.to_hex().c_str());

  if (cache.exists(quicr_name)) {
    // duplicate, ignore
    DEBUG("Duplicate message Name: %s", quicr_name.to_hex().c_str());
    return;

  } else {
    cache.put(quicr_name, data);
  }

  std::list<Subscriptions::Remote> list = subscribeList->find(quicr_name);

  // TODO: Send to peers (aka other relays)

  for (auto dest : list) {
    DEBUG("Sending object '%s' to subscriber %d", quicr_name.to_hex().c_str(),
          dest.subscribe_id);

    quicr::bytes copy = data;
    server->sendNamedObject(dest.subscribe_id, quicr_name, 0, 0, false,
                            std::move(copy));
  }
}

void ClientManager::onPublishedFragment(
    const quicr::Name &quicr_name, uint8_t priority, uint16_t expiry_age_ms,
    bool use_reliable_transport, const uint64_t &offset, bool is_last_fragment,
    quicr::bytes &&data) {}

void ClientManager::onSubscribe(const quicr::Namespace &quicr_namespace,
                                const uint64_t &subscriber_id,
                                const quicr::SubscribeIntent subscribe_intent,
                                const std::string &origin_url,
                                bool use_reliable_transport,
                                const std::string &auth_token,
                                quicr::bytes &&data) {

  DEBUG("onSubscribe namespace: %s/%d", quicr_namespace.to_hex().c_str(),
        quicr_namespace.length());

  Subscriptions::Remote remote = {.subscribe_id = subscriber_id};
  subscribeList->add(quicr_namespace.name(), quicr_namespace.length(), remote);

  // respond with response
  auto result = quicr::SubscribeResult{
      quicr::SubscribeResult::SubscribeStatus::Ok, "", {}, {}};
  server->subscribeResponse(quicr_namespace, 0x0, result);
};

} // namespace laps