
#include "peer_manager.h"

namespace laps {
PeerManager::PeerManager(const Config &cfg) : config(cfg) {
  logger = cfg.logger;
}

} // namespace laps