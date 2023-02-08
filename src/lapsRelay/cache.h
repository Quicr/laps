#pragma once

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <quicr/quicr_common.h>

#include "config.h"

namespace laps {

/**
 * @brief Thread-safe cache of named object data
 */
class Cache {
public:
  Cache(const Config &cfg);

  void put(const quicr::Name &name, const std::vector<uint8_t> &data);

  const std::vector<uint8_t> get(const quicr::Name &name);

  bool exists(const quicr::Name &name);

  std::list<quicr::Name> find(const quicr::Name &name, const int len);

  ~Cache();

private:
  typedef std::map<quicr::Name, std::vector<uint8_t>> CacheMap;

  std::mutex w_mutex;

  const Config &config;
  Logger *logger;

  int CacheMaxBuffers;  // Max number of cache buffers
  int CacheMapCapacity; // Max capacity for cache map

  int cacheBufferPos; // Current cache buffer that is active
  std::map<int, CacheMap>
      cacheBuffer; // Cache buffers. Acts like an array of data cache buffers
  const std::vector<uint8_t> emptyVec;
};
} // namespace laps