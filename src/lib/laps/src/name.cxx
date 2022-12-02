
#include <assert.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string.h>


#include <name.h>
#include <laps.h>

Name::Name(MsgShortName &n) {
    name.msgShortName = n;
};

Name::Name(NamePath path, uint32_t org, uint32_t team, uint16_t channel,
           uint32_t device, uint32_t msgNum) { // name for message data

  assert(path == NamePath::message);
  assert(org <= 0x3ffff);    // 18 bits
  assert(team <= 0xFffff);   // 20 bits
  assert(channel <= 0x3FF);  // 10 bits
  assert(device <= 0xFffff); // 20 bits
  assert(msgNum <= 0xFffff); // 20 bits

  // When setting the values from long network byte order, shift to the right the value so that it fits in the bits allocated

  name.spec.origin_id = htonl(itad) >> 8;
  name.spec.app_id = qmsgAppID;
  name.spec.path = (uint8_t) path;
  name.spec.org = htonl(org) >> 14;
  name.spec.team = htonl(team) >> 12;
  name.spec.channel = htonl(channel) >> 22;
  name.spec.device = htonl(device) >> 12;
  name.spec.msg_id = htonl(msgNum) >> 12;
}

uint32_t Name::orginID() {
  return ntohl((uint32_t)(name.spec.origin_id) << 8);
};
uint8_t Name::appID() {
  return (uint8_t)(name.spec.app_id);
};
NamePath Name::path() {
  return (NamePath)(name.spec.path);
};
uint32_t Name::org() {
  return ntohl((uint32_t)(name.spec.org) << 14);
};
uint32_t Name::team() {
  return ntohl((uint32_t)(name.spec.team) << 12);
};
uint32_t Name::channel() {
  return ntohl((uint32_t)(name.spec.channel) << 22);
};

uint32_t Name::device() {
  return ntohl((uint32_t)(name.spec.device) << 12);
};

uint32_t Name::msgNum() {
  return ntohl((uint32_t)(name.spec.msg_id) << 12);
};

MsgShortName Name::shortName() {
  MsgShortName n;
  std::memcpy(n.data, name.data, sizeof(n.data));
  return n;
}

std::string Name::shortString() {
  return getMsgShortNameHexString(name.data);
}

std::string Name::longString() {
  std::stringstream ss;
  if ( ( orginID() != itad ) || ( appID() != qmsgAppID ) ) {
    ss << "unknown-" << shortString() ;
  } else {
    switch ( path() ) {
    case NamePath::message: {
      ss << "origin_id: " << orginID()
         << " qmsg://msg"
         << "/org-" << org()
         << "/team-" << team()
         << "/ch-" << channel()
         << "/dev-" << device() 
         << "/msg-" << msgNum();
    }
      break;
    default:
      break;
    }
  }
  
  return ss.str();
};
