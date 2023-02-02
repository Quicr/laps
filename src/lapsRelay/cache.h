
#include <list>
#include <map>
#include <set>
#include <vector>

#include "Logger.h"
#include <laps.h>

class Cache {
public:
  Cache(Logger *logPtr);
  void put(const PubName &name, const std::vector<uint8_t> &data);

  const std::vector<uint8_t> *get(const PubName &name) const;
  bool exists(const PubName &name) const;

  std::list<PubName> find(const PubName &name, const int len) const;

  ~Cache();

private:
  typedef std::map<PubName, std::vector<uint8_t>> CacheMap;

  Logger *logger; // Logging class pointer

  // TODO: Add these to config
  int CacheMaxBuffers;  // Max number of cache buffers
  int CacheMapCapacity; // Max capacity for cache map

  int cacheBufferPos; // Current cache buffer that is active
  std::map<int, CacheMap>
      cacheBuffer; // Cache buffers. Acts like an array of data cache buffers
  const std::vector<uint8_t> emptyVec;
};
