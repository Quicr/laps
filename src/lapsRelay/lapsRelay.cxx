
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <laps.h>
#include <name.h>

#include "subscription.h"
#include "cache.h"


int main(int argc, char* argv[]) {
  std::clog << "Starting LAPS Relay (laps version " << lapsVersion() << ")" << std::endl;
  SlowerConnection slower;

  int port = lapsDefaultPort;
  char* portVar = getenv( "LAPS_PORT" );
  if ( portVar ) {
      port = atoi( portVar );
  }

  int err = slowerSetup( slower, port );
  assert( err == 0 );

  // ========  Setup up upstream and relay mesh =========
  std::list<SlowerRemote>  relays;
  for ( int i=1; i< argc; i++ ) {
     SlowerRemote relay;
     err = slowerRemote( relay , argv[i] );
     if ( err ) {
       std::cerr << "Could not lookup IP address for relay: " << argv[i]  << std::endl;
     } else {
       std::clog << "Using relay at " << inet_ntoa( relay.addr.sin_addr)  << ":" <<  ntohs(relay.addr.sin_port) << std::endl;
       relays.push_back( relay );
     }
  }
  // get relays from ENV var
  char* relayEnv = getenv( "LAPS_RELAYS" );
  if ( relayEnv ) {
    std::string rs( relayEnv );
    std::stringstream ss( rs );
    std::vector<std::string> relayNames;

    std::string buf;
    while ( ss >> buf ) {
      relayNames.push_back( buf );
    }
    
    for ( auto r : relayNames ) {
      SlowerRemote relay;
      err = slowerRemote( relay , (char*)r.c_str(), port );
      if ( err ) {
        std::cerr << "Could not lookup IP address for relay: " << r  << std::endl;
      } else {
        std::clog << "Using relay at " << inet_ntoa( relay.addr.sin_addr)  << ":" <<  ntohs(relay.addr.sin_port) << std::endl;
        relays.push_back( relay );
      }
    }
  }
  

  // ========== MAIN Loop ==============
  Subscriptions subscribeList;
  Cache cache;

  while (true ) {
    //std::cerr << "Waiting ... ";
    err=slowerWait( slower );
    assert( err == 0 );
    //std::cerr << "done" << std::endl;
  
    char buf[slowerMTU];
    int bufLen=0;
    SlowerRemote remote;
    MsgHeader mhdr = {0};
    MsgHeaderMetrics metrics = {0};
    int len;
    
    err=slowerRecvMulti(  slower, &mhdr, &remote, &len,  buf, sizeof(buf), &bufLen, &metrics );

    if (err != 0) {
      std::clog << "Error reading message" << std::endl;
      continue;
    }

    // =========  PUBLISH ===================
    if ( ( mhdr.type == SlowerMsgPub ) && ( bufLen > 0 ) ) {
      bool duplicate = cache.exists( mhdr.name );

      if (duplicate) {
        std::clog << "Got "
                  << (duplicate ? "dup " : "")
                  << "PUB " << Name(mhdr.name).longString()
                  << " from " << inet_ntoa(remote.addr.sin_addr)
                  << ":" << ntohs(remote.addr.sin_port)
                  << std::endl;
      }

      err = slowerAck( slower, mhdr.name, &remote );
      assert( err == 0 );
        
      if ( !duplicate ) {
        // add to local cache 
        std::vector<uint8_t> data(buf, buf + bufLen);

//        std::clog << "Adding to cache: " << getMsgShortNameHexString(mhdr.name.data) <<  std::endl;

        cache.put(mhdr.name, data);

        // report metrics for QMsg
        if (mhdr.flags.metrics) {
          Name qmsgName(mhdr.name);
          metrics.relay_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();

//          std::clog << "    " << qmsgName.longString() << " pub latency: " << (metrics.relay_millis - metrics.pub_millis)
//                    << std::endl;
        }

        // send to other relays
        for (SlowerRemote dest: relays) {
          if (dest != remote) {
//            std::clog << "  Sent to relay " << inet_ntoa(dest.addr.sin_addr) << ":" << ntohs(dest.addr.sin_port)
//                      << std::endl;
            err = slowerPub(slower, mhdr.name, buf, bufLen, &dest, mhdr.flags.metrics ? &metrics : NULL);
            assert(err == 0);
          }
        }

        // send to anyone subscribed
        std::list<SlowerRemote> list = subscribeList.find(mhdr.name);

        //std::clog << "subscribers " << list.size() << std::endl;

        for (SlowerRemote dest: list) {
           if (dest != remote) {
//            std::clog << "  Sent to subscriber " << inet_ntoa(dest.addr.sin_addr) << ":" << ntohs(dest.addr.sin_port)
//                      << std::endl;
            err = slowerPub(slower, mhdr.name, buf, bufLen, &dest, mhdr.flags.metrics ? &metrics : NULL);
            assert(err == 0);
          }
        }
      }
    }

    // ============ SUBSCRIBE ==================
    if ( mhdr.type == SlowerMsgSub  ) {
      std::clog << "Got SUB" 
                << " for " << Name(mhdr.name).longString()  << " " << len
                << " from " << inet_ntoa( remote.addr.sin_addr) << ":" <<  ntohs( remote.addr.sin_port )
                << " shortname: " << getMsgShortNameHexString(mhdr.name.data)
                << std::endl;
      subscribeList.add( mhdr.name, len, remote );

      // TODO: change if and what we send from cache
      /*
      std::list<MsgShortName> names = cache.find(mhdr.name, len );
      // names.reverse(); // send the highest (and likely most recent) first

      for ( auto n : names ) {
        std::clog << "  Sent cache " << Name(n).longString() << std::dec << std::endl;
        const std::vector<uint8_t>* priorData = cache.get( n );

        if ( priorData->size() != 0 ) {
          err = slowerPub( slower, n, (char*)(priorData->data()), priorData->size(), &remote );
          assert( err == 0 );
        } else {
             std::clog << "    PROBLEM with missing priorData" << std::endl;
        } 
      }
      */
  
    }

    // ============== Un SUBSCRIBE ===========
    if ( mhdr.type == SlowerMsgUnSub  ) {
       std::clog << "Got UnSUB" 
                 << " for " <<  Name(mhdr.name).longString() << "*" << len
                 << " from " << inet_ntoa( remote.addr.sin_addr) << ":" <<  ntohs( remote.addr.sin_port )
                 << std::endl;
       subscribeList.remove( mhdr.name, len, remote );
    }  
  }

  slowerClose( slower );
  return 0;
}
