
#include <list>
#include <set>
#include <map>
#include <vector>

#include <laps.h>
#include "Logger.h"

class Cache {
public:
	Cache(Logger *logPtr);
  void put(const MsgShortName& name, const std::vector<uint8_t>& data );

  const std::vector<uint8_t>* get( const MsgShortName& name ) const;
  bool exists(  const MsgShortName& name ) const;

  std::list<MsgShortName> find(const MsgShortName& name, const int len ) const;

  ~Cache();
  
private:
	typedef std::map<MsgShortName, std::vector<uint8_t> > CacheMap;

	Logger                      *logger;            // Logging class pointer


	// TODO: Add these to config
	int                         CacheMaxBuffers;    // Max number of cache buffers
	int                         CacheMapCapacity;   // Max capacity for cache map

	int                         cacheBufferPos;     // Current cache buffer that is active
	std::map<int, CacheMap>     cacheBuffer;        // Cache buffers. Acts like an array of data cache buffers
  const std::vector<uint8_t>  emptyVec;
};
