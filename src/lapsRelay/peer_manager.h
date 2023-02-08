#pragma once

#include "config.h"

namespace laps {
class PeerManager {
public:
  PeerManager(const Config &cfg);

private:
  const Config &config;
  Logger *logger;
};
} // namespace laps