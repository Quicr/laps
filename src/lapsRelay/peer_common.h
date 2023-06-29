#pragma once

#include <quicr/encode.h>
#include <quicr/namespace.h>
#include <transport/safe_queue.h>

namespace laps {
    using namespace qtransport;
    using namespace quicr;

    using nameList = std::vector<Namespace>;

    enum class PeerObjectType : uint8_t {
        PUBLISH = 0,
        SUBSCRIBE,
        UNSUBSCRIBE
    };

    struct PeerObject {
        PeerObjectType  type;                             /// Object type
        std::string     source_peer_id;                   /// Peer ID if from peer, otherwise empty

        messages::PublishDatagram pub_obj;                /// published object to send
        Namespace       sub_namespace;                    /// [Un]Subscribe namespace
    };

    using peerQueue = safeQueue<PeerObject>;


 } // namespace laps