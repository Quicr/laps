#pragma once

#include <quicr/encode.h>
#include <quicr/quicr_namespace.h>
#include <transport/safe_queue.h>

namespace laps {
    using namespace qtransport;
    using namespace quicr;

    using nameList = std::vector<Namespace>;

    struct PeerObject {
        std::string source_peer_id;                       /// Peer ID if from peer, otherwise empty
        messages::PublishDatagram pub_obj;                /// published object to send
    };

    using peerQueue = safeQueue<PeerObject>;


    /**
     * @brief Subscribe context
     */
    struct SubscribeContext {
        bool                filtered { false };          /// Indicates if the subscription is to be filtered or not
        std::string         peer_id;                     /// ID of the remote peer (can be used to lookup peer context

        std::shared_ptr<ITransport> transport;           /// Transport associated to the subscription
        TransportContextId          tctx_id;             /// Transport context ID
        StreamId                    stream_id;           /// Stream ID for the subscription to use
    };

    using PeerSubscriptions = namespace_map<std::vector<SubscribeContext>>;


} // namespace laps