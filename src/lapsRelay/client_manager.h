#pragma once

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "cache.h"
#include "config.h"
#include "subscription.h"

namespace laps {

/**
 * @brief Thread-safe cache of named object data
 */
class ClientManager : public quicr::ServerDelegate {
public:
  ~ClientManager();

  ClientManager(const Config &cfg, Cache &cache);

  void start();

  /*
   * Overloads
   */
  void onPublishIntent(const quicr::Namespace &quicr_name,
                       const std::string &origin_url,
                       bool use_reliable_transport,
                       const std::string &auth_token,
                       quicr::bytes &&e2e_token) override;

  void onPublisherObject(const quicr::Name &quicr_name,
                         const qtransport::TransportContextId &context_id,
                         const qtransport::MediaStreamId &stream_id,
                         uint8_t priority, uint16_t expiry_age_ms,
                         bool use_reliable_transport,
                         quicr::bytes &&data) override;

  void onPublishedFragment(const quicr::Name &quicr_name, uint8_t priority,
                           uint16_t expiry_age_ms, bool use_reliable_transport,
                           const uint64_t &offset, bool is_last_fragment,
                           quicr::bytes &&data) override;

  void onSubscribe(const quicr::Namespace &quicr_namespace,
                   const uint64_t &subscriber_id,
                   const qtransport::TransportContextId& context_id,
                   const qtransport::MediaStreamId& stream_id,
                   const quicr::SubscribeIntent subscribe_intent,
                   const std::string &origin_url, bool use_reliable_transport,
                   const std::string &auth_token, quicr::bytes &&data) override;

private:
  Subscriptions *subscribeList;
  const Config &config;
  Cache &cache;
  Logger *logger;

  std::unique_ptr<quicr::QuicRServer> server;
  std::shared_ptr<qtransport::ITransport> transport;
  std::set<uint64_t> subscribers = {};
};
} // namespace laps