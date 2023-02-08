
#include <iostream>

#include "cache.h"
#include "config.h"

namespace laps {
Cache::Cache(const Config &cfg) : config(cfg) {

  logger = cfg.logger;
  CacheMaxBuffers = cfg.cache_max_buffers;
  CacheMapCapacity = cfg.cache_map_capacity;

  // Current buffer starts at zero
  cacheBufferPos = 0;

  // initialize the buffers
  for (int i = 0; i <= CacheMaxBuffers; i++) {
    cacheBuffer[i].clear();
  }
}

void Cache::put(const quicr::Name &name, const std::vector<uint8_t> &data) {
  std::lock_guard<std::mutex> lock(w_mutex);

  // Move to next buffer if current buffer is at capacity
  if ((int)cacheBuffer[cacheBufferPos].size() >= CacheMapCapacity) {
    DEBUG("Current buffer %d full, moving to next buffer", cacheBufferPos);
    cacheBufferPos++;

    // Wrap the buffer position if at the end
    if (cacheBufferPos >= CacheMaxBuffers) {
      DEBUG("Buffer wrapped");
      cacheBufferPos = 0;
    }

    // Clear the previous buffer cache if it is not empty
    if (cacheBuffer[cacheBufferPos].size() > 0) {
      DEBUG("Clearing oldest buffer");
      cacheBuffer[cacheBufferPos].clear();
    }
  }

  cacheBuffer[cacheBufferPos].emplace(name, data);
}

const std::vector<uint8_t> Cache::get(const quicr::Name &name) {

  std::lock_guard<std::mutex> lock(w_mutex);

  // Check all buffers as if they are one
  for (int i = 0; i <= CacheMaxBuffers; i++) {
    auto mapPtr = cacheBuffer.at(i).find(name);

    if (mapPtr != cacheBuffer.at(i).end()) {
      // Return found data
      return mapPtr->second;
    }
  }

  return emptyVec;
}

bool Cache::exists(const quicr::Name &name) {
  std::lock_guard<std::mutex> lock(w_mutex);

  // Check all buffers as if they are one
  for (int i = 0; i <= CacheMaxBuffers; i++) {
    auto mapPtr = cacheBuffer.at(i).find(name);

    // quicr::Name name = quicr::Name(const_cast<quicr::Name &>(mapPtr->first));

    if (mapPtr != cacheBuffer.at(i).end()) {
      // Return found data
      return true;
    }
  }

  return false;
}

std::list<quicr::Name> Cache::find(const quicr::Name &name, const int len) {
  std::list<quicr::Name> ret;

  const quicr::Name &startName = quicr::Namespace(name, len).name();

  quicr::Name endName(startName);

  // Set the endName to be the highest value (all ones of non-significant bits)
  if (len < 128) {
    u_char non_sig_bits = 128 - len;
    endName >>= non_sig_bits; // Shift significant bits right, removing
                              // non-significant bits

    // Shift back setting to 1 non-significant bits
    while (non_sig_bits-- > 0) {
      endName <<= 1;
      endName += 1;
    }
  }

  std::lock_guard<std::mutex> lock(w_mutex);

  // Check all buffers as if they are one
  for (int i = 0; i <= CacheMaxBuffers; i++) {
    auto start = cacheBuffer.at(i).lower_bound(startName);
    auto end = cacheBuffer.at(i).upper_bound(endName);

    for (auto it = start; it != end; it++) {
      auto dataName = it->first;

      ret.push_back(dataName);
    }
  }

  return ret;
}

Cache::~Cache() {
  for (auto it = cacheBuffer.begin(); it != cacheBuffer.end(); it++) {
    it->second.clear();
  }

  cacheBuffer.clear();
}
} // namespace laps