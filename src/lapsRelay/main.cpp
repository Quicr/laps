#include <chrono>
#include <sstream>

#include <transport/safe_queue.h>
#include <quicr/quicr_name.h>
#include "cache.h"
#include "client_manager.h"
#include "config.h"
#include "version_config.h"

using namespace laps;
using namespace qtransport;
using namespace quicr;

static Logger *logger;

std::string lapsVersion() { return LAPS_VERSION; }

int main(int /* argc */, char *[] /* argv[] */) {
  Config cfg;
  logger = cfg.logger;

  LOG_INFO("Starting LAPS Relay (version %s)", lapsVersion().c_str());

  ClientSubscriptions subscriptions(cfg);
  Cache cache(cfg);
  safeQueue<messages::PublishDatagram> peer_queue;

  // Start UDP client manager
  ClientManager udp_mgr(cfg, cache, subscriptions, peer_queue);
  udp_mgr.start();

  // Start QUIC client manager using the UDP port plus one
  cfg.client_port++;

  if (cfg.tls_cert_filename == NULL) {
    cfg.tls_cert_filename = "./server-cert.pem";
    cfg.tls_key_filename = "./server-key.pem";
  }

  cfg.protocol = RelayInfo::Protocol::QUIC;

  ClientManager quic_mgr(cfg, cache, subscriptions, peer_queue);
  quic_mgr.start();

  while (quic_mgr.ready() && udp_mgr.ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }

}
