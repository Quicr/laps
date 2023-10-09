#pragma once

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <quicr/encode.h>
#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "cache.h"
#include "client_subscriptions.h"
#include "peer_manager.h"
#include "config.h"

namespace laps {

/**
 * @brief Thread-safe cache of named object data
 */
class ClientManager : public quicr::ServerDelegate {
  private:
    ClientManager(const Config& cfg, Cache& cache,
                  ClientSubscriptions& subscriptions,
                  peerQueue &peer_queue);

  public:

  [[nodiscard]] static std::shared_ptr<ClientManager> create(const Config& cfg, Cache& cache,
                                                             ClientSubscriptions& subscriptions,
                                                             peerQueue &peer_queue)
  {
      return std::shared_ptr<ClientManager>(new ClientManager(cfg, cache, subscriptions, peer_queue));
  }

  /*
   * Overloads
   */

  quicr::PublishIntentResult onPublishIntent(const quicr::Namespace& quicr_name,
                                             const std::string& origin_url,
                                             bool use_reliable_transport,
                                             const std::string& auth_token,
                                             quicr::bytes&& e2e_token) override;

  void onPublishIntentEnd(const quicr::Namespace& quicr_namespace,
                          const std::string& auth_token,
                          quicr::bytes&& e2e_token) override;

  std::vector<quicr::PublishResult> onPublisherObject(const qtransport::TransportContextId& context_id,
                                                      const qtransport::StreamId& stream_id,
                                                      bool use_reliable_transport,
                                                      const quicr::messages::PublishDatagram& datagram) override;

  quicr::SubscribeResult onSubscribe(const quicr::Namespace& quicr_namespace,
                                     const uint64_t& subscriber_id,
                                     const qtransport::TransportContextId& context_id,
                                     const qtransport::StreamId& stream_id,
                                     const quicr::SubscribeIntent subscribe_intent,
                                     const std::string& origin_url,
                                     bool use_reliable_transport,
                                     const std::string& auth_token,
                                     quicr::bytes&& data) override;

  void onUnsubscribe(const quicr::Namespace& quicr_namespace,
                     const uint64_t& subscriber_id,
                     const std::string& auth_token) override;

  const ClientSubscriptions& getSubscriptions() const { return subscribeList; }

private:
  cantina::LoggerPointer logger;
  ClientSubscriptions& subscribeList;
  const Config &config;
  Cache &cache;
  const uint16_t client_mgr_id;             /// This client mgr ID, uses listening port

  std::shared_ptr<qtransport::ITransport> transport;
  std::set<uint64_t> subscribers = {};
  peerQueue& _peer_queue;

};
} // namespace laps
