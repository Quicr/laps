#ifndef LAPS_H
#define LAPS_H

#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstring>

#include "version_config.h"

// TODO - flip this to be a class

std::string lapsVersion();

const uint16_t lapsDefaultPort = 33434;
const uint16_t slowerMTU = 1500;

typedef struct {
  socklen_t addrLen;
  struct sockaddr_in addr;
} SlowerRemote;

typedef struct {
  int fd;  // UDP socket
  SlowerRemote relay;
} SlowerConnection;

/**
 * ShortName named path values
 *     See https://github.com/Quicr/qmsg/blob/main/doc/protocol.md for more details.
 */
enum class NamePath : uint8_t {
    message = 1,
    keyPackage = 2,
    welcome = 3,
    commitAll = 4,
    commitOne = 5
};

/**
 * Defines the "MsgShortName" binary header. This is the binary
 *     encoded name for published messages. Subscribers subscribe using bits/len,
 *     where name_length defines the number of significant bits.
 */
#define MSG_SHORT_NAME_LEN    16
typedef struct {
    u_char       data[MSG_SHORT_NAME_LEN];       ///< Buffer for the spec struct, this must be the size of the packed spec
} __attribute__ ((__packed__, __aligned__(1))) MsgShortName;

/**
 * Defines the "MsgShortName" binary header. This is the binary
 *     encoded name from string qmsg://msg/org-<org>/team-<team>/ch-<channel>/dev-<device>/<msgNum>
 *
 *     See https://github.com/Quicr/qmsg/blob/main/doc/protocol.md for more details.
 */
 typedef union {
     struct { // 128 bits / 16 bytes

         uint32_t         origin_id: 24;          ///< Origin ID
         u_char           app_id;                 ///< App ID
         u_char           path;                   ///< Path as defined by enum NamePath
         uint32_t         org: 18;                ///< Org

         uint32_t         team: 20;               ///< Team
         uint16_t         channel: 10;            ///< Channel
         uint32_t         device: 20;             ///< Device
         uint32_t         msg_id: 20;             ///< Message ID

     } __attribute__ ((__packed__, __aligned__(1))) spec;

     MsgShortName        msgShortName;            ///< Common Message shortName

     u_char       data[MSG_SHORT_NAME_LEN];       ///< Buffer for the spec struct, this must be the size of the packed spec
 } __attribute__ ((__packed__, __aligned__(1))) QMsgShortName;
 

// TODO: Add Key Package and Welcome ShortNames

/**
 * Message types
 */
typedef enum {
    SlowerMsgInvalid =0,
    SlowerMsgPub=1,
    SlowerMsgSub=2,
    SlowerMsgUnSub=3,
    SlowerMsgAck=4
} SlowerMsgType;

/**
 * Message header flags - 1 Byte
 */
struct MsgHeaderFlags {
    u_char        metrics : 1;                ///< Set to indicate if metrics are included in the header.
    u_char        reserved: 7;                ///< Unused/remaining bits
} __attribute__ ((__packed__, __aligned__(1)));

/**
 * Defines the slow-relay message header. Every message header must start with this. Data follows
 *     the header.
 */
struct MsgHeader {
    int8_t                        type;                 ///< Message type as defined by enum SlowerMsgType
    MsgHeaderFlags                flags;                ///< Flags as defined by MsgHeaderFlags struct
    MsgShortName                  name;
} __attribute__ ((__packed__, __aligned__(1)));

/**
 * Message Header metrics
 */
struct MsgHeaderMetrics {
    uint64_t                      pub_millis;           ///< Publisher/sender time since epoch in milliseconds
    uint64_t                      relay_millis;         ///< Sending Relay time since epoch in milliseconds

    // TODO: Add variable array for tracing end-to-end hops
};

/**
 * Defines the slow-relay publish message header. This follows the slow-relay message header.
 */
struct MsgPubHeader {
    uint16_t                        dataLen;              ///< Length of the data to follow
} __attribute__ ((__packed__, __aligned__(1)));

/**
 * Defines the slow-relay subscribe message header. This follows the slow-relay message header.
 */
struct MsgSubHeader {
    uint8_t                         name_length;              ///< Subscriber name_length
} __attribute__ ((__packed__, __aligned__(1)));


bool operator==(const MsgShortName& a, const MsgShortName& b );
bool operator!=(const MsgShortName& a, const MsgShortName& b );
bool operator<(const MsgShortName& a, const MsgShortName& b );

bool operator!=( const SlowerRemote& a, const SlowerRemote& b );
bool operator<( const SlowerRemote& a, const SlowerRemote& b );




int slowerSetup( SlowerConnection& slower, const uint16_t port=0);
int slowerRemote( SlowerRemote& remote, const char server[], const uint16_t port=lapsDefaultPort );
int slowerAddRelay( SlowerConnection& slower, const SlowerRemote& remote );
int slowerClose( SlowerConnection& slower );

int slowerWait( SlowerConnection& slower );

int slowerGetFD( SlowerConnection& slower);

int slowerPub(SlowerConnection& slower, const MsgShortName& name, char buf[], int bufLen,
              SlowerRemote* remote=NULL, MsgHeaderMetrics *metrics=NULL);
int slowerAck(SlowerConnection& slower, const MsgShortName& name, SlowerRemote* remote=NULL );
int slowerSub(SlowerConnection& slower, const MsgShortName& name, int len, SlowerRemote* remote=NULL );
int slowerUnSub(SlowerConnection& slower, const MsgShortName& name, int len, SlowerRemote* remote=NULL );

int slowerRecvPub(SlowerConnection& slower, MsgHeader* msgHeader, char buf[], int bufSize, int* bufLen,
                  MsgHeaderMetrics *metrics=NULL);
int slowerRecvAck(SlowerConnection& slower, MsgShortName* name  );
int slowerRecvMulti(SlowerConnection& slower, MsgHeader *msgHeader, SlowerRemote* remote,
                    int* len, char buf[], int bufSize, int* bufLen, MsgHeaderMetrics *metrics=NULL );
void getMaskedMsgShortName(const MsgShortName &src, MsgShortName &dst, const int len);
std::string getMsgShortNameHexString(const u_char *data);

#endif  // LAPS_H
