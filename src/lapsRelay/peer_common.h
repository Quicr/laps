#pragma once

#include <quicr/encode.h>
#include <quicr/namespace.h>
#include <transport/safe_queue.h>

namespace laps {
    using namespace qtransport;
    using namespace quicr;

    using nameList = std::vector<Namespace>;
    using peer_id_t = std::string;

    enum class PeerObjectType : uint8_t {
        PUBLISH = 0,
        SUBSCRIBE,
        UNSUBSCRIBE,
        PUBLISH_INTENT,
        PUBLISH_INTENT_DONE,
    };

    struct PeerObject {
        PeerObjectType  type;                             /// Object type
        peer_id_t       source_peer_id;                   /// Peer ID if from peer, otherwise empty
        peer_id_t       origin_peer_id;                   /// Origin of a publish intent

        messages::PublishDatagram pub_obj;                /// published object to send
        Namespace nspace {};                              /// Subscribe or publish intent namespace
    };

    using peerQueue = safeQueue<PeerObject>;


 } // namespace laps