
#include <cassert>
#include <iostream>
#include <cstring>

#include "cache.h"


void Cache::put(const MsgShortName& name, const std::vector<uint8_t>& data ) {
  assert( data.size() > 0 );

  std::vector<uint8_t>* vec = new std::vector<uint8_t>( data );

  std::pair<MsgShortName, std::vector<uint8_t>* > pair;
  pair = make_pair( name, vec );

  dataCache.insert( pair );

  // Prune the cache if needed
  if (dataCache.size() > 1000000) {
        dataCache.erase(std::next(dataCache.begin(), 1), std::next(dataCache.begin(), 40000));
  }

}


const std::vector<uint8_t>* Cache::get( const MsgShortName& name ) const {
  auto mapPtr = dataCache.find( name );
  if ( mapPtr == dataCache.end() ) {
    return &emptyVec;
  }

  const std::vector<uint8_t>* vecP =  mapPtr->second;
  assert( vecP );
  assert( vecP->size() > 0 );

  return  vecP;
}


bool Cache::exists(  const MsgShortName& name ) const {
  auto mapPtr = dataCache.find( name );
  if ( mapPtr == dataCache.end() ) {
    return false;
  }
  return true; 
}


std::list<MsgShortName> Cache::find(const MsgShortName& name, const int len ) const {
  std::list<MsgShortName> ret;

  MsgShortName startName = name;

  getMaskedMsgShortName(name, startName, len);

  MsgShortName endName =  startName;

  // Set the non-significant bits to 1
  u_char sig_bytes = len / 8; // example 121 / 8 = 15 bytes significant, one bit in the last byte is significant

  // Set all 8 bits for bytes that should be set.
  if (sig_bytes < MSG_SHORT_NAME_LEN - 1) {
    std::memset(endName.data + sig_bytes, 0xff, MSG_SHORT_NAME_LEN - sig_bytes - 1);
  }

  // Handle the last byte of the significant bits. For example, /110 has 13 significant bytes + 1 bit of byte 14
  //   that is significant.  The last byte is byte 14 for a /110
  if (len % 8 > 0) {
    u_char mask_byte = 0xff >> (len % 8);
    endName.data[sig_bytes] |= mask_byte;

  } else {
    endName.data[sig_bytes] = 0xff;
  }

  auto start = dataCache.lower_bound( startName );
  auto end = dataCache.upper_bound( endName );
  
  for ( auto it = start; it != end; it++ ) {
    MsgShortName dataName = it->first;

    assert( it->second );
    assert( it->second->size() > 0 );
    
    ret.push_back( dataName );
  }
  
  return ret;
}


Cache::~Cache(){
  for( auto it =  dataCache.begin(); it != dataCache.end(); it++ ) {
    assert( it->second );
    delete it->second;
    dataCache.erase( it );
  }
  dataCache.clear();
}
