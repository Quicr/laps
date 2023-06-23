
#include <quicr/quicr_client.h>
#include <sstream>
#include "peer_manager.h"

namespace laps {

PeerManager::PeerManager(const Config& cfg,
                         TransportConfig& transportConfig,
                         safeQueue<messages::PublishDatagram>& peer_queue,
                         Cache& cache,
                         ClientSubscriptions &subscriptions)
    : _config(cfg), _peer_queue(peer_queue),
      _transport_config(transportConfig),
      _cache(cache),
      _subscriptions(subscriptions)
{
  logger = cfg.logger;

  _client_rx_msg_thr = std::thread(&PeerManager::ClientRxThread, this);
}

PeerManager::~PeerManager() {
  // Stop threads
  _stop = true;

  logger->log(LogLevel::info, "Closing peer manager threads");

  // Clear threads from the queue
  _peer_queue.stopWaiting();

  if (_client_rx_msg_thr.joinable())
    _client_rx_msg_thr.join();

  logger->log(LogLevel::info, "Closed peer manager threads");
  logger->log(LogLevel::info, "Closed peer manager stopped");
}

void PeerManager::StartPeerSession(RelayInfo& peer_params,
                                   const nameList& pub_names,
                                   const nameList& sub_names)
{
  auto& client = _peers_sessions.emplace_back(std::make_unique<QuicRClient>(
      peer_params, _transport_config, *logger));

  std::ostringstream log_msg;

  for (const auto& pub_ns: pub_names) {
    client->publishIntent(std::shared_ptr<PublisherDelegate>(this),
        pub_ns, {}, {}, {});
  }

  for (const auto& sub_ns: sub_names) {
    client->subscribe(std::shared_ptr<SubscriberDelegate>(this),
                          sub_ns, {}, {}, false, {}, {});
  }

}

void PeerManager::ClientRxThread() {
  logger->log(LogLevel::info, "Running peer manager client receive thread");

  while (not _stop) {
    const auto &obj = _peer_queue.block_pop();

    if (obj) {
      logger->log(LogLevel::info, "Received publish message name: " + obj->header.name.to_hex());
    }
  }
}

/*
 * Delegate Implementations
 */
void PeerManager::onSubscribedObject(const Name &quicr_name, uint8_t priority,
                        uint16_t expiry_age_ms, bool use_reliable_transport,
                        bytes &&data) {}


} // namespace laps