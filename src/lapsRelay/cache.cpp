
#include <cstring>
#include <iostream>
#include <memory>

#include "logger.h"
#include "cache.h"
#include "config.h"

namespace laps {
Cache::Cache(const Config &cfg) : config(cfg) {

  stop = false;
  logger = std::make_shared<cantina::Logger>("CACHE", cfg.logger);
  CacheMaxBuffers = cfg.cache_max_buffers;
  CacheMapCapacity = cfg.cache_map_capacity;

  // Current buffer starts at zero
  cacheBufferPos = 0;

  // initialize the buffers
  for (uint i = 0; i < CacheMaxBuffers; i++) {
    cacheBuffer[i].clear();
  }

  cache_mon_thr = std::thread([this] { monitor_thread(); });
}

void Cache::monitor_thread() {
  logger->Log("Running cache monitor thread");

  std::unique_lock<std::mutex> lock(w_mutex);
  lock.unlock();

  while (not stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(config.cache_expire_ms));

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

    for (const auto &entry : cacheBuffer) {

      if (not entry.second.empty()) {
        uint64_t ms;
        std::memcpy(&ms, entry.second.at(CACHE_INFO_NAME).at(0).data(), 8);

        if (now_ms - ms > config.cache_expire_ms) {
            FLOG_DEBUG("cache i: " << entry.first << " time: " << ms
                       << " is over max ms age " << (now_ms - ms) << " > "
                       << config.cache_expire_ms << ", purging cache");

            lock.lock();

            cacheBuffer[cacheBufferPos].clear();

            lock.unlock();
        }
      }
    }
  }
}

void Cache::put(const quicr::Name &name, unsigned int offset,
                const std::vector<uint8_t> &data) {
  std::lock_guard<std::mutex> lock(w_mutex);

  // Move to next buffer if current buffer is at capacity
  if (cacheBuffer[cacheBufferPos].size() >= CacheMapCapacity) {
    FLOG_DEBUG("Current buffer " << cacheBufferPos << " full, moving to next buffer");
    cacheBufferPos++;

    // Wrap the buffer position if at the end
    if (cacheBufferPos >= CacheMaxBuffers) {
      FLOG_DEBUG("Buffer wrapped");
      cacheBufferPos = 0;
    }

    // Clear the moved-to buffer cache if it is not empty
    if (cacheBuffer[cacheBufferPos].size() > 0) {
      FLOG_DEBUG("Clearing oldest buffer");
      cacheBuffer[cacheBufferPos].clear();
    }
  }

  if (cacheBuffer[cacheBufferPos].size() == 0) {
    // Add cache specific named object to track time of creation
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    FLOG_DEBUG("New cache buffer " << cacheBufferPos << " time is " << ms);
    std::vector<uint8_t> time;
    time.resize(8);
    std::memcpy(time.data(), &ms, 8);
    cacheBuffer[cacheBufferPos][CACHE_INFO_NAME].emplace(0, time);
  }

  cacheBuffer[cacheBufferPos][name].emplace(offset, data);
}

const Cache::CacheMapEntry Cache::get(const quicr::Name &name) {

  std::lock_guard<std::mutex> lock(w_mutex);

  // Check all buffers as if they are one
  uint i = cacheBufferPos;
  do {
    if (cacheBuffer[i].count(name) > 0) {
      return cacheBuffer[i][name];
    }

    if (++i >= CacheMaxBuffers) // Wrap to start if hit end of buffers
      i = 0;

  } while (i != cacheBufferPos);

  return emptyCacheMapEntry;
}

bool Cache::exists(const quicr::Name &name, unsigned int offset) {
  std::lock_guard<std::mutex> lock(w_mutex);

  // Check all buffers as if they are one
  uint i = cacheBufferPos;
  do {

    if (cacheBuffer.at(i).count(name) > 0 &&
        cacheBuffer[i][name].count(offset) > 0) {
      return true;
    }

    if (++i >= CacheMaxBuffers)
      i = 0;

  } while (i != cacheBufferPos);

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
  uint i = cacheBufferPos;
  do {
    auto start = cacheBuffer.at(i).lower_bound(startName);
    auto end = cacheBuffer.at(i).upper_bound(endName);

    for (auto it = start; it != end; it++) {
      auto dataName = it->first;

      ret.push_back(dataName);
    }

    if (++i >= CacheMaxBuffers)
      i = 0;

  } while (i != cacheBufferPos);

  return ret;
}

Cache::~Cache() {
  stop = true;

  logger->info << "Cache stopped" << std::flush;
  if (cache_mon_thr.joinable()) {
    cache_mon_thr.detach(); // join hangs on monitor thread sleep. Detach is fine in this case
  }

  for (auto it = cacheBuffer.begin(); it != cacheBuffer.end(); it++) {
    it->second.clear();
  }

  cacheBuffer.clear();
}
} // namespace laps
