#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include <quicr/encode.h>
#include <quicr/quicr_namespace.h>
#include <quicr/quicr_client.h>
#include <quicr/quicr_client_delegate.h>
#include <transport/safe_queue.h>
#include "config.h"
#include "cache.h"
#include "client_subscriptions.h"

namespace laps {
using namespace qtransport;
using namespace quicr;



class PeerManager : public SubscriberDelegate, public PublisherDelegate {
public:
  using peerQueue = safeQueue<messages::PublishDatagram>;

  PeerManager(const Config& cfg,
              TransportConfig& transportConfig,
              safeQueue<messages::PublishDatagram>& peer_queue,
              Cache& cache,
              ClientSubscriptions &subscriptions);

  ~PeerManager();

  /*
   * Delegate functions
   */
  void onSubscribeResponse(const Namespace &quicr_namespace,
                           const SubscribeResult &result) override {}

  void onSubscriptionEnded(
      const Namespace &quicr_namespace,
      const SubscribeResult::SubscribeStatus &reason) override {}

  void onSubscribedObject(const Name &quicr_name, uint8_t priority,
                          uint16_t expiry_age_ms, bool use_reliable_transport,
                          bytes &&data) override;

  void onPublishIntentResponse(const Namespace &quicr_namespace,
                               const PublishIntentResult &result) override {}


private:
  using nameList = std::vector<Namespace>;

  /**
   * @brief Client thread to monitor client published messages
   */
  void ClientRxThread();

  /**
   * @brief Start a peering session/connection using QuicrClient
   *
   * @param relayInfo           Relay/peer connect parameters
   */
   void StartPeerSession(RelayInfo& peer_params,
                         const nameList& pub_names,
                         const nameList& sub_names);

 private:

  std::atomic<bool> _stop { false };
  const Config& _config;
  TransportConfig& _transport_config;
  peerQueue& _peer_queue;
  Cache& _cache;
  ClientSubscriptions& _subscriptions;

  std::thread _client_rx_msg_thr;                            /// Client receive message thread

  std::vector<std::unique_ptr<QuicRClient>> _peers_sessions; /// Peer sessions

  Logger *logger;

};

} // namespace laps