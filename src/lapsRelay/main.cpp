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

  Cache cache(cfg);
  ClientManager client_mgr(cfg, cache);

  client_mgr.start();

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }
}
