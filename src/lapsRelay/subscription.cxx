

#include <cassert>
#include <iostream>

#include <laps.h>

#include "subscription.h"


Subscriptions::Subscriptions() {
  subscriptions.resize(129);
}

void Subscriptions::add(const PubName& name, const int len, const SlowerRemote& remote ) {
  PubName group;

  bzero(group.data, sizeof(group.data));
  getMaskedMsgShortName(name, group, len);

//  std::clog << "add subscription len=" << len
//      << " with key " << getMsgShortNameHexString(name.data)
//      << std::endl;

  auto mapPtr =  subscriptions[len].find(group );
  if ( mapPtr == subscriptions[len].end() ) {
    std::set<SlowerRemote> list;
    list.insert( remote );
    std::pair<PubName,std::set<SlowerRemote>> pair;
    pair = make_pair( group , list );
    subscriptions[len].insert(pair );
  }
  else {
    std::set<SlowerRemote>& list = mapPtr->second;
    if ( list.find( remote ) == list.end() ) {
      list.insert( remote ); // TODO - rethink if list is right thing here 
    }
  }
}
  
void Subscriptions::remove(const PubName& name, const int len, const SlowerRemote& remote ) {
  PubName group;
  getMaskedMsgShortName(name, group, len);

  auto mapPtr = subscriptions[len].find(group );
  if ( mapPtr != subscriptions[len].end() ) {
    std::set<SlowerRemote>& list = mapPtr->second;
    if ( list.find( remote ) == list.end() ) {
      list.erase( remote ); 
    }
  }
}
  
std::list<SlowerRemote> Subscriptions::find(  const PubName& name  ) {
  std::list<SlowerRemote> ret;
  PubName group;

  // TODO: Fix this to not have to iterate for each mask bit
  for ( int len=0; len <= 128 ; len++ ) {
//    std::clog << "Looking up masked entry len: " << len;
//    std::clog << " : ";

    getMaskedMsgShortName(name, group, len);

//    std::clog << getMsgShortNameHexString(group.data) << std::endl;

    auto mapPtr = subscriptions[len].find( group );
    if ( mapPtr != subscriptions[len].end() ) {
      std::set<SlowerRemote>& list = mapPtr->second;
      for( const SlowerRemote& remote : list ) {
        SlowerRemote dest = remote;
        ret.push_back( dest );
        //std::clog << "subscribe.cxx:     found port=" <<  ntohs( remote.addr.sin_port )<< std::endl;
      }
    }
  }

  return ret;
}


