#pragma once

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <atomic>

#include <quicr/quicr_common.h>

#include "config.h"

namespace laps {

/**
 * @brief Thread-safe cache of named object data
 */
class Cache {
public:
  typedef std::map<uint, std::vector<uint8_t>> CacheMapEntry;
  typedef std::map<quicr::Name, CacheMapEntry> CacheMap;

  Cache(const Config &cfg);

  void put(const quicr::Name &name, unsigned int offset,
           const std::vector<uint8_t> &data);

  const CacheMapEntry get(const quicr::Name &name);

  bool exists(const quicr::Name &name, unsigned int offset);

  std::list<quicr::Name> find(const quicr::Name &name, const int len);

  ~Cache();

private:
  void monitor_thread();

  const quicr::Name CACHE_INFO_NAME{"0x00000000000000000000000000000000"};
  const uint MAX_CACHE_MS_AGE = 45000;

  std::atomic<bool> stop;
  std::mutex w_mutex;
  std::thread cache_mon_thr;

  const Config &config;
  Logger *logger;

  uint CacheMaxBuffers;  // Max number of cache buffers
  uint CacheMapCapacity; // Max capacity for cache map

  uint cacheBufferPos; // Current cache buffer that is active
  std::map<uint, CacheMap>
      cacheBuffer; // Cache buffers. An array of data cache buffers
  const CacheMapEntry emptyCacheMapEntry;
  const CacheMap emptyCacheMap;
};
} // namespace laps