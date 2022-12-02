
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netdb.h>
#include <stdio.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>


#ifdef __linux__
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>
#else
#include <net/if_dl.h>
#endif

#include <laps.h>

static int slowerRecv( SlowerConnection& slower, char buf[], int bufSize, int* bufLen, SlowerRemote* remote );
static int slowerSend( SlowerConnection& slower, char buf[], int bufLen, SlowerRemote& remote );

std::string lapsVersion() {
  return LAPS_VERSION;
}


bool operator!=( const SlowerRemote& a, const SlowerRemote& b ){
  if ( a.addr.sin_port != b.addr.sin_port ) return true;
  if ( a.addrLen != b.addrLen ) return true;
  if ( memcmp( &(a.addr.sin_addr), &(b.addr.sin_addr),4 ) != 0)  return true; // TODO broken for v6
  return false;
}

bool operator<( const SlowerRemote& a, const SlowerRemote& b ){
  if ( a.addr.sin_port > b.addr.sin_port ) return true;
  if ( a.addr.sin_port < b.addr.sin_port ) return false;
  if ( a.addrLen > b.addrLen ) return true;
  if ( a.addrLen < b.addrLen ) return false;
  if ( memcmp( &(a.addr.sin_addr), &(b.addr.sin_addr),4 ) > 0)  return true; // TODO broken for v6
  return false;
}


bool operator<(const MsgShortName& a, const MsgShortName& b )
{
  return (std::memcmp(&a.data, &b.data, sizeof(MsgShortName)) < 0 );
}

bool operator==(const MsgShortName& a, const MsgShortName& b ) {
  return (std::memcmp(&a.data, &b.data, sizeof(MsgShortName)) == 0 );
}

bool operator!=(const MsgShortName& a, const MsgShortName& b ){
  return (std::memcmp(&a.data, &b.data, sizeof(MsgShortName)) == 0 );
}

std::string getMsgShortNameHexString(const u_char *data) {
  char hexStr[37] = {0};
  char *hexStrPtr = hexStr;

  for (int i=0; i < MSG_SHORT_NAME_LEN; i++) {
    if (i % 4 == 0 && i != 0) {
      *hexStrPtr++ = '-';
    }

    snprintf(hexStrPtr, 3, "%02X", data[i]);
	  //printf("i = %d hex: %s data: %02X\n", i, hexStr, data[i]);
    hexStrPtr += 2;
  }

  return std::string(hexStr);
}

void getMaskedMsgShortName(const MsgShortName &src, MsgShortName &dst, const int len) {

  // Set the data len to the size of bytes to keep at 8 bit boundaries.  The last byte is a wildcard
  //   that will be added back so that it can be masked.
  u_char dst_len = len > 0 ? (len / 8) : 0;
  u_char dst_bits = len % 8;

  bzero(&dst, MSG_SHORT_NAME_LEN);

  if (dst_len == 0) {
    return;
  }

  std::memcpy(dst.data, src.data, dst_len);   // Copy all bytes to keep as-is on 8bit boundaries

  // Handle the last byte
  if (dst_bits) {
    dst.data[dst_len] = src.data[dst_len] >> (8 - dst_bits) << (8 - dst_bits);
  }
}


int slowerSetup( SlowerConnection& slower, uint16_t port) {
  slower.fd=0;
  bzero( &slower.relay, sizeof( slower.relay ) );
  
  int err;
  
  slower.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (slower.fd < 0) {
    perror("problem opening socket");
    exit(-1);
  }

  if ( port != 0 ) {
    int optionValue = 1;
    err = setsockopt(slower.fd, SOL_SOCKET, SO_REUSEPORT, (const void *)&optionValue,
                     sizeof(optionValue));
    if (err != 0) {
        perror("problem setting socket re-use option");
        exit(-2);
    }

    struct sockaddr_in srvAddr;
    bzero((char *)&srvAddr, sizeof(srvAddr));
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(port);
    srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    std::clog << "Listening on 0.0.0.0:" << port << std::endl;
    err = bind(slower.fd, (struct sockaddr *)&srvAddr, sizeof(srvAddr));
    if (err < 0) {
      perror("problem on binding to port to listen on");
      exit(-3);
    }
  }

  struct timeval timeOut;
  timeOut.tv_sec = 0;
  timeOut.tv_usec = 10 * 1000;
  int serr = setsockopt( slower.fd, SOL_SOCKET, SO_RCVTIMEO, &timeOut, sizeof(timeOut));
  if (serr) {
    std::cerr << "Problem setting timeout on socket" << std::endl;
  }

  return 0;
}

int slowerRemote(  SlowerRemote& remote, const char* server, const uint16_t port ){
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED | AI_ALL;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  // Get IP address of remote 
  std::string portString = std::to_string(port);
  struct addrinfo* results=NULL;
  int err = getaddrinfo(server, portString.c_str() , &hints, &results);
  if (err) {
    perror("Failed to lookup remote host");
    return -1; 
  }

  struct addrinfo* result=NULL;
  for ( result = results; result != NULL; result = result->ai_next) {
    if (result->ai_addrlen > sizeof(remote.addr)) continue;
    
    memset(&remote.addr, 0, sizeof(remote.addr));
    assert(result->ai_addrlen <= sizeof(remote.addr));

    remote.addrLen = results->ai_addrlen;
    memcpy(&remote.addr, results->ai_addr, result->ai_addrlen);
#if 0
    std::clog << "Got remote IP of " << inet_ntoa( remote.addr.sin_addr) << std::endl;
#endif
    break; // found a good result 
  }
  if ( result == NULL ) {
    // no result found
     freeaddrinfo(results);
     return -1;
  }
  
  freeaddrinfo(results);
  return 0;
}

static int slowerSend( SlowerConnection& slower, char buf[], int bufLen, SlowerRemote* remote ) {
  assert( slower.fd > 0 );

  SlowerRemote dest;
  if ( remote == NULL ) {
    dest = slower.relay;
  } else {
    dest = *remote;
  }

#if 0
  std::clog << "in slowerSend with"
            << " bufLen=" << bufLen
            << " IP=" << inet_ntoa( dest.addr.sin_addr)
            << " port=" << ntohs( dest.addr.sin_port )
            << " remote=" << ( (remote)?1:0 ) 
            << std::endl;
#endif
    
  int n = sendto( slower.fd, buf, bufLen, 0 /*flags*/,
                  (struct sockaddr*)&dest.addr, dest.addrLen);
  if (n < 0) {
    perror("UDP sendto error");
    return -1;
  } else if (n != bufLen) {
    perror("UDP sendto failed");
    return -1;
  }
  
  return 0;
}

int slowerGetFD( SlowerConnection& slower) {
  return slower.fd;
}

int slowerWait( SlowerConnection& slower ){
  pollfd pfd;

  pfd.fd = slower.fd;
  pfd.events = POLLIN | POLLPRI | POLLHUP | POLLERR;
  pfd.revents = 0;

  int err = poll(&pfd, 1, 50);

  if ( err < 0 ) {
    perror("Error on select");
    return -1;
  }

  return 0;
}

static int slowerRecv( SlowerConnection& slower, char buf[], int bufSize, int* bufLen, SlowerRemote* remote ) {
  assert( slower.fd > 0 );

  bzero( buf, bufSize );

  bzero( &(remote->addr) , sizeof(  remote->addr ) );
  remote->addrLen = sizeof(  remote->addr );
  *bufLen = 0; 


  //ssize_t r = recv(slower.fd, (void*)buf, (size_t)bufSize, 0 /*flags*/) ;
  ssize_t r = recvfrom(slower.fd, (void*)buf, (size_t)bufSize, 0 /*flags*/,
                       (struct sockaddr *)&(remote->addr),
                       &(remote->addrLen)
                       );
  if ( r == -1 ) {
    int e = errno ;
    if ( e == EAGAIN ) {
      return 0;
    }
    std::cerr << "revc udp packet got error: " << strerror(e) << std::endl;
     return -1;
   }
   *bufLen = r ;

#if 0
   std::clog << "in slowerRecv with bufLen=" << *bufLen << std::endl;
#endif
   
   return 0;
}


int slowerAddRelay( SlowerConnection& slower, const SlowerRemote& remote ){
  slower.relay = remote;
  return 0;
}

  
int slowerPub(SlowerConnection& slower, const MsgShortName& name, char buf[], int bufLen,
              SlowerRemote* remote, MsgHeaderMetrics *metrics) {
  assert( bufLen < slowerMTU-20 ); 
  assert( slower.fd > 0 );
  assert( bufLen > 0 );
  
  char msg[slowerMTU];
  int msgLen=0;

  MsgHeader mhdr = {0};
  mhdr.type = SlowerMsgPub;
  mhdr.flags.metrics = metrics == NULL ? 0 : 1;
  mhdr.name = name;

  memcpy(msg+msgLen, &mhdr, sizeof(mhdr)); msgLen += sizeof(mhdr);

  if (mhdr.flags.metrics) {
    memcpy(msg + msgLen, metrics, sizeof(MsgHeaderMetrics)); msgLen += sizeof(MsgHeaderMetrics);
    assert( msgLen + bufLen < sizeof(msg) );
  }

  MsgPubHeader mpub_hdr;
  mpub_hdr.dataLen = bufLen;
  assert( sizeof(msg) - msgLen >= sizeof(mpub_hdr));
  memcpy(msg+msgLen, &mpub_hdr, sizeof(mpub_hdr)); msgLen += sizeof(mpub_hdr);
  assert( msgLen + bufLen < sizeof(msg) );

  memcpy( msg+msgLen, buf, bufLen ) ; msgLen += bufLen;
  assert( msgLen < sizeof( msg ) );

  int err = slowerSend( slower, msg, msgLen, remote );
  return err;
}


int slowerRecvMulti(SlowerConnection& slower, MsgHeader *msgHeader, SlowerRemote* remote,
                    int* len, char buf[], int bufSize, int* bufLen, MsgHeaderMetrics *metrics ){

  assert (msgHeader);
  assert( remote );
  assert( len );
  assert( buf );
  assert( bufLen );
  assert( bufSize > 0 );

  msgHeader->type = SlowerMsgInvalid;
  *len=0;
  *bufLen=0;
  bzero( msgHeader->name.data, sizeof( msgHeader->name.data ) );

  char msg[slowerMTU];
  int msgLen=0; // total name_length of data received
  int msgLoc=0; // position of current decode of messages
  
  int err = slowerRecv( slower, msg, sizeof(msg), &msgLen, remote );
  if ( err != 0 ) {
    return err;
  }
  if ( msgLen == 0 ) {
    return 0;
  }

  memcpy(msgHeader, msg, sizeof(MsgHeader));
  msgLoc += sizeof(MsgHeader);

  if (msgHeader->type == SlowerMsgInvalid)
    return -1;

  if (msgHeader->flags.metrics) {
    if (metrics != NULL)
      memcpy(metrics, msg+msgLoc, sizeof(MsgHeaderMetrics));

    msgLoc += sizeof(MsgHeaderMetrics);
  }

//  std::clog << "MSG HDR:" << std::endl
//      << " Type       : " << SlowerMsgType(mhdr.type) << std::endl
//      << " MsgShortName  : " << std::endl
//      << " -------------------------------" << std::endl
//      << "   Origin   : " << mhdr.name.spec.origin_id << std::endl
//      << "   App ID   : " << (int) mhdr.name.spec.app_id << std::endl
//      << "   Path     : " << (int) mhdr.name.spec.path << std::endl
//      << "   Org      : " << mhdr.name.spec.org << std::endl
//      << "   Team     : " << mhdr.name.spec.team << std::endl
//      << "   Channel  : " << mhdr.name.spec.channel << std::endl
//      << "   Device   : " << mhdr.name.spec.device << std::endl
//      << "   Msg ID   : " << mhdr.name.spec.msg_id << std::endl;

  switch (msgHeader->type) {
  case SlowerMsgPub:
    MsgPubHeader mpub_hdr;

    if (bufSize - msgLoc < sizeof(mpub_hdr)) {
      std::clog << "Not enough bytes read for message header size, dropping" << std::endl;
      return -1;
    }

    memcpy(&mpub_hdr, msg+msgLoc, sizeof(mpub_hdr)); msgLoc += sizeof(mpub_hdr);

    //std::clog << "msg size: " << mpub_hdr.dataLen << std::endl;

    if (bufSize - msgLoc < mpub_hdr.dataLen) {
      std::clog << "Not enough bytes read for length of data" << std::endl;
      return -1;
    }

    memcpy( buf, msg+msgLoc, mpub_hdr.dataLen ); msgLoc += mpub_hdr.dataLen;
    *bufLen = mpub_hdr.dataLen;

    assert( msgLoc == msgLen );
    break;

  case SlowerMsgSub:
    MsgSubHeader msub_hdr;

    assert ( bufSize - msgLoc >= sizeof(msub_hdr));
    memcpy(&msub_hdr, msg+msgLoc, sizeof(msub_hdr)); msgLoc += sizeof(msub_hdr);

    *len = msub_hdr.name_length;
    assert( msgLoc == msgLen );
    break;

  case SlowerMsgUnSub:
    assert( msgLoc == msgLen );
    break;

  case SlowerMsgAck:
    assert( msgLoc == msgLen );
    break;

  default:
    return -1;
  }

  return 0;
}

int slowerRecvAck(SlowerConnection& slower, MsgShortName* name ){
  assert( name );

  MsgHeader mhdr = {0};
  SlowerRemote remote;
  int mask;

  char buf[slowerMTU];
  int bufSize=sizeof(buf);
  int bufLen=0;

  int err = slowerRecvMulti( slower, &mhdr, &remote, &mask, buf, bufSize, &bufLen );
  if ( err != 0 ) {
    return err;
  }

  memcpy(name->data, mhdr.name.data, MSG_SHORT_NAME_LEN);

  if ( mhdr.type != SlowerMsgAck ) {
    bzero( name, sizeof( *name ) );
    return 0;
  }
  
  return 0;
}


int slowerRecvPub(SlowerConnection& slower, MsgHeader* msgHeader, char buf[], int bufSize, int* bufLen, MsgHeaderMetrics *metrics ){
  assert( msgHeader );
  assert( bufLen );
  assert( bufSize > 0 );
  
  SlowerRemote remote;
  int len;
  
  int err = slowerRecvMulti(slower, msgHeader, &remote, &len, buf, bufSize, bufLen, metrics );
  if ( err != 0 ) {
    return err;
  }

  if ( msgHeader->type != SlowerMsgPub ) {
    *bufLen = 0;
    return 0;
  }
  
  return 0;
}


int slowerAck(SlowerConnection& slower, const MsgShortName& name, SlowerRemote* remote ){
  assert( slower.fd > 0 );
  
  char msg[slowerMTU];
  int msgLen=0;

  MsgHeader mhdr = {0};
  mhdr.type = SlowerMsgAck;
  mhdr.name = name;

  memcpy( msg+msgLen, &mhdr, sizeof(mhdr) ) ; msgLen += sizeof(mhdr);

  assert( msgLen < sizeof( msg ) );
          
  int err = slowerSend( slower, msg, msgLen, remote );
  return err;
}

int slowerSub(SlowerConnection& slower, const MsgShortName& name, int len , SlowerRemote* remote ){
  assert( slower.fd > 0 );
  assert(len >= 0 );
  assert(len <= 128 );
  
  char msg[slowerMTU];
  int msgLen=0;

  MsgHeader mhdr = {0};
  mhdr.type = SlowerMsgSub;
  mhdr.name = name;

  memcpy( msg+msgLen, &mhdr, sizeof(mhdr) ) ; msgLen += sizeof(mhdr);

  assert( msgLen < sizeof( msg ) );

  MsgSubHeader msub_hdr;
  msub_hdr.name_length = len;

  assert(msgLen + sizeof(msub_hdr) < sizeof (msg));

  memcpy( msg+msgLen, &msub_hdr, sizeof(msub_hdr) ) ; msgLen += sizeof(msub_hdr);

  assert( msgLen < sizeof( msg ) );
          
  int err = slowerSend( slower, msg, msgLen, remote );
  return err;
}


int slowerUnSub(SlowerConnection& slower, const MsgShortName& name, int len , SlowerRemote* remote  ) {
  assert( slower.fd > 0 );
  assert(len >= 0 );
  assert(len <= 128 );
  
  char msg[slowerMTU];
  int msgLen=0;

  MsgHeader mhdr = {0};
  mhdr.type = SlowerMsgUnSub;
  mhdr.name = name;

  memcpy( msg+msgLen, &mhdr, sizeof(mhdr) ) ; msgLen += sizeof(mhdr);

  assert( msgLen < sizeof( msg ) );

  MsgSubHeader msub_hdr;
  msub_hdr.name_length = len;

  assert(sizeof(msg) < msgLen + sizeof(msub_hdr));

  memcpy( msg+msgLen, &msub_hdr, sizeof(msub_hdr) ) ; msgLen += sizeof(msub_hdr);

  assert( msgLen < sizeof( msg ) );


  int err = slowerSend( slower, msg, msgLen, remote );
  return err;
}


int slowerClose( SlowerConnection& slower ){
  close( slower.fd ); slower.fd=0;
  bzero( & slower.relay, sizeof( slower.relay ) );
  
  return 0;
}
