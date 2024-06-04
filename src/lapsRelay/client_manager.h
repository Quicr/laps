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
class ClientManager : public quicr::ServerDelegate, public std::enable_shared_from_this<ClientManager> {
  private:
    ClientManager() = delete;

    ClientManager(const Config& cfg, Cache& cache,
                  ClientSubscriptions &subscriptions,
                  peerQueue &peer_queue);

  public:
  std::shared_ptr<ClientManager> get_shared_ptr()
  {
      return shared_from_this();
  }

  [[nodiscard]] static std::shared_ptr<ClientManager> create(const Config& cfg, Cache& cache,
                                                             ClientSubscriptions &subscriptions,
                                                             peerQueue &peer_queue)
  {
      return std::shared_ptr<ClientManager>(new ClientManager(cfg, cache, subscriptions,peer_queue));
  }

  ~ClientManager();


  void start();
  void stop();
  bool ready();

  /*
   * Overloads
   */

  void onPublishIntent(const quicr::Namespace &quicr_name,
                       const std::string &origin_url,
                       const std::string &auth_token,
                       quicr::bytes &&e2e_token) override;

  void onPublishIntentEnd(const quicr::Namespace &quicr_namespace, const std::string &auth_token,
                          quicr::bytes &&e2e_token) override;

  void onPublisherObject(const qtransport::TransportConnId& conn_id,
                         const qtransport::DataContextId& data_ctx_id,
                         bool reliable,
                         quicr::messages::PublishDatagram &&datagram) override;

  void onSubscribe(const quicr::Namespace &quicr_namespace,
                   const uint64_t& subscriber_id,
                   const qtransport::TransportConnId& conn_id,
                   const qtransport::DataContextId& data_ctx_id,
                   const quicr::SubscribeIntent subscribe_intent,
                   const std::string &origin_url,
                   const std::string &auth_token, quicr::bytes &&data) override;

  void onSubscribePause(const quicr::Namespace& quicr_namespace,
                        const uint64_t subscriber_id,
                        const qtransport::TransportConnId conn_id,
                        const qtransport::DataContextId data_ctx_id,
                        const bool pause) override;

  void onUnsubscribe(const quicr::Namespace &quicr_namespace,
                     const uint64_t &subscriber_id,
                     const std::string &auth_token) override;


private:
  cantina::LoggerPointer logger;
  ClientSubscriptions &subscribeList;
  const Config &config;
  Cache &cache;
  const uint16_t client_mgr_id;             /// This client mgr ID, uses listening port
  bool running {false};

  std::unique_ptr<quicr::Server> server;
  std::shared_ptr<qtransport::ITransport> transport;
  std::set<uint64_t> subscribers = {};
  peerQueue& _peer_queue;

};
} // namespace laps
