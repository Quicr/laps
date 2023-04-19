#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "cache.h"
#include "client_manager.h"
#include "config.h"
#include "version_config.h"

using namespace laps;

static Logger *logger;

std::string lapsVersion() { return LAPS_VERSION; }

int main(int /* argc */, char *[] /* argv[] */) {
  Config cfg;
  logger = cfg.logger;

  LOG_INFO("Starting LAPS Relay (version %s)", lapsVersion().c_str());

  /*
// ========  Setup up upstream and relay mesh =========
std::list<SlowerRemote>  relays;
for ( int i=1; i< argc; i++ ) {
SlowerRemote relay;
err = slowerRemote( relay , argv[i] );
if ( err ) {
 LOG_ERR("Could not lookup IP address for relay: %s", argv[i]);

} else {
                   LOG_INFO("Using relay at %s:%d", inet_ntoa(
relay.addr.sin_addr), ntohs(relay.addr.sin_port)); relays.push_back( relay );
}
}
// get relays from ENV var
char* relayEnv = getenv( "LAPS_RELAYS" );
if ( relayEnv ) {
std::string rs( relayEnv );
std::stringstream ss( rs );
std::vector<std::string> relayNames;

std::string buf;
while ( ss >> buf ) {
relayNames.push_back( buf );
}

for ( auto r : relayNames ) {
SlowerRemote relay;
err = slowerRemote( relay , (char*)r.c_str(), port );
if ( err ) {
  LOG_ERR("Could not lookup IP address for relay: %s", r.c_str());
} else {
  LOG_INFO("Using relay at %s: %d", inet_ntoa( relay.addr.sin_addr),
ntohs(relay.addr.sin_port)); relays.push_back( relay );
}
}
}
  */

  ClientSubscriptions subscriptions(cfg);
  Cache cache(cfg);

  // Start UDP client manager
  ClientManager udp_mgr(cfg, cache, subscriptions);
  udp_mgr.start();

  // Start QUIC client manager using the UDP port plus one
  cfg.client_port++;

  if (cfg.tls_cert_filename == NULL) {
    cfg.tls_cert_filename = "./server-cert.pem";
    cfg.tls_key_filename = "./server-key.pem";
  }

  cfg.protocol = quicr::RelayInfo::Protocol::QUIC;

  ClientManager quic_mgr(cfg, cache, subscriptions);
  quic_mgr.start();

  while (quic_mgr.ready() && udp_mgr.ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }

}
