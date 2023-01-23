
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <iomanip>

#include <chrono>

#include <laps.h>
#include <name.h>


int main(int argc, char* argv[]) {
  if ( ( argc != 2 ) && (  argc != 3 ) ) {
    std::string lapsVer = lapsVersion();
    std::cerr << "Relay address and port set in LAPS_RELAY and LAPS_PORT env variables as well as LAPS_ORG" << std::endl;
    std::cerr <<  std::endl; 
    std::cerr << "Usage PUB: lapsTest FF0001 pubData" << std::endl; 
    std::cerr << "Usage SUB: lapsTest FF0000:16" << std::endl;
    std::cerr << "lapsTest lib version " << lapsVer << std::endl;
    exit(-1);
  }
  
  char* relayName =  getenv( "LAPS_RELAY" );
  if ( !relayName ) {
    static  char defaultRelay[]  = "relay.us-east-2.media10x.ctgpoc.com ";
    relayName = defaultRelay;
  }
   
  uint32_t org = 1;
  char* orgVar = getenv( "LAPS_ORG" ); 
  if ( orgVar ) {
    org = atoi( orgVar );
  }

  int port = 33434;
  char* portVar = getenv( "LAPS_PORT" ); 
  if ( portVar ) {
    port = atoi( portVar );
  }

  std::string nameString( argv[1] );
	PubName msn {0 };

	int len = 0;

	try {

		int pos = 0;
		for (int i=0; i < strlen(argv[1]); i+=2) {
			msn.data[pos++] = std::stoul( nameString.substr(i, 2 ), nullptr, 16);
    }

  } catch ( ... ) {
    std::clog << "invalid input name: " << nameString <<  std::endl;
    exit (1);
  }
 
  std::cerr << "Name = " << Name( msn ).shortString()
            << std::endl;
  
    
  std::vector<uint8_t> data;
  if ( argc == 3 ) {
    data.insert( data.end(), (uint8_t*)(argv[2]) , ((uint8_t*)(argv[2])) + strlen( argv[2] ) );
  }

  
  SlowerConnection slower;
  int err = slowerSetup( slower  );
  assert( err == 0 );
  
  SlowerRemote relay;
  err = slowerRemote( relay , relayName, port );
  if ( err ) {
    std::cerr << "Could not lookup IP address for relay: " << relayName << std::endl;
  }
  assert( err == 0 );
  std::clog << "Using relay at " << inet_ntoa( relay.addr.sin_addr)  << ":" <<  ntohs(relay.addr.sin_port) << std::endl;

  
  err = slowerAddRelay( slower, relay );
  assert( err == 0 );

  
  if ( data.size() > 0  ) {
    // do publish
    std::clog << "PUB to "
              <<  Name( msn ).shortString()
              << " aka "
              << Name( msn ).longString()
              << std::endl;
    MsgHeaderMetrics metrics = {0};

    metrics.pub_millis = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::system_clock::now().time_since_epoch() ).count();


    err = slowerPub( slower,  msn,  (char*)data.data() , data.size(), NULL, &metrics);
    assert( err == 0 );

     while ( true ) {
      err=slowerWait( slower );
      assert( err == 0 );

      PubName recvName;
      err = slowerRecvAck( slower, &recvName );
      assert( err == 0 );
      if ( recvName == msn ) {
        std::clog << "   Got ACK for " << Name( recvName ).longString() << std::endl;
        break;
      }
     }
  }
  else {
    // do subscribe 
    std::clog << "SUB to "
              <<  Name( msn ).shortString() << ":" << len
              << " aka "
              << Name( msn ).longString()   << ":" << len
              << std::endl;

    err = slowerSub( slower,  msn, len  );
    assert( err == 0 );

    while ( true ) {
      err=slowerWait( slower );
      assert( err == 0 );

      char buf[slowerMTU];
      int bufLen=0;
      MsgHeader mhdr;
      MsgHeaderMetrics metrics = {0};
      
      err = slowerRecvPub( slower, &mhdr, buf, sizeof(buf), &bufLen, &metrics);
      uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::system_clock::now().time_since_epoch() ).count();

      assert( err == 0 );

      if (bufLen > 0) {
        std::clog << "Got data for "
                  << Name( mhdr.name ).longString() << " ";
          //<< " len=" << bufLen

        if (metrics.pub_millis and metrics.relay_millis) {
          std::clog << std::endl
                    << "  pub age ms       : " << (nowMs - metrics.pub_millis) << std::endl
                    << "  relay latency ms : " << (nowMs - metrics.relay_millis) << std::endl;
        }

        std::clog << "  data --> " ;

        for ( int i=0; i< bufLen; i++ ) {
          char c = buf[i];
          if (( c >= 32 ) && ( c <= 0x7e ) ) {
            std::clog << c;
          }
          else {
             std::clog << '~';
          }
        }

        std::clog  << std::endl;
        // break;
      }
    }
    
    err = slowerUnSub( slower, msn, len);
    assert( err == 0 );
  }
    
  slowerClose( slower );
  return 0;
}
