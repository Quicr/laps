
#include <cassert>
#include <iostream>
#include <cstring>

#include "Logger.h"
#include "cache.h"
#include "name.h"


Cache::Cache(Logger *logPtr) {
	logger = logPtr;


	// TODO: Add config for buffers
	CacheMaxBuffers = 10;
	CacheMapCapacity = 200000;

	// Current buffer starts at zero
	cacheBufferPos = 0;

	// initialize the buffers
	for (int i=0; i <= CacheMaxBuffers; i++) {
		cacheBuffer[i].clear();
	}

}

void Cache::put(const PubName& name, const std::vector<uint8_t>& data ) {

	// Move to next buffer if current buffer is at capacity
	if (cacheBuffer[cacheBufferPos].size() >= CacheMapCapacity) {
		LOG_INFO("Current buffer %d full, moving to next buffer", cacheBufferPos);
		cacheBufferPos++;

		// Wrap the buffer position if at the end
		if (cacheBufferPos >= CacheMaxBuffers) {
			LOG_INFO("Buffer wrapped");
			cacheBufferPos = 0;
		}

		// Clear the previous buffer cache if it is not empty
		if (cacheBuffer[cacheBufferPos].size() > 0) {
			LOG_INFO("Clearing oldest buffer");
			cacheBuffer[cacheBufferPos].clear();
		}
	}

	cacheBuffer[cacheBufferPos].emplace(name, data);
}


const std::vector<uint8_t>* Cache::get( const PubName& name ) const {

	// Check all buffers as if they are one
	for (int i = 0; i <= CacheMaxBuffers; i++) {
		auto mapPtr = cacheBuffer.at(i).find(name);

		if ( mapPtr != cacheBuffer.at(i).end() ) {
			// Return found data
			return &mapPtr->second;
		}
	}

	return &emptyVec;
}


bool Cache::exists(  const PubName& name ) const {

	// Check all buffers as if they are one
	for (int i = 0; i <= CacheMaxBuffers; i++) {
		auto mapPtr = cacheBuffer.at(i).find(name);

		Name name = Name(const_cast<PubName&>(mapPtr->first));

		if ( mapPtr != cacheBuffer.at(i).end() ) {
			// Return found data
			return true;
		}

	}

  return false;
}


std::list<PubName> Cache::find(const PubName& name, const int len ) const {
  std::list<PubName> ret;

  PubName startName = name;

  getMaskedMsgShortName(name, startName, len);

  PubName endName =  startName;

  // Set the non-significant bits to 1
  u_char sig_bytes = len / 8; // example 121 / 8 = 15 bytes significant, one bit in the last byte is significant

  // Set all 8 bits for bytes that should be set.
  if (sig_bytes < PUB_NAME_LEN - 1) {
    std::memset(endName.data + sig_bytes, 0xff, PUB_NAME_LEN - sig_bytes - 1);
  }

  // Handle the last byte of the significant bits. For example, /110 has 13 significant bytes + 1 bit of byte 14
  //   that is significant.  The last byte is byte 14 for a /110
  if (len % 8 > 0) {
    u_char mask_byte = 0xff >> (len % 8);
    endName.data[sig_bytes] |= mask_byte;

  } else {
    endName.data[sig_bytes] = 0xff;
  }

	// Check all buffers as if they are one
	for (int i = 0; i <= CacheMaxBuffers; i++) {
		auto start = cacheBuffer.at(i).lower_bound(startName);
		auto end = cacheBuffer.at(i).upper_bound(endName);

		for (auto it = start; it != end; it++) {
			PubName dataName = it->first;

			ret.push_back(dataName);
		}
	}
  
  return ret;
}

Cache::~Cache(){
  for( auto it =  cacheBuffer.begin(); it != cacheBuffer.end(); it++ ) {
    it->second.clear();
  }

	cacheBuffer.clear();
}
