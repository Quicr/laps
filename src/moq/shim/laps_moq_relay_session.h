#pragma once

#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQTypes.h>

#include <proxygen/lib/http/webtransport/WebTransport.h>

namespace laps {

    /**
     * @brief MoQRelaySession with laps-only helpers; avoids patching vendored moxygen for relay control writes.
     */
    class LapsMoqRelaySession final : public moxygen::MoQRelaySession
    {
      public:
        using MoQRelaySession::MoQRelaySession;

        /**
         * Sends PUBLISH_NAMESPACE_OK (REQUEST_OK) for the given announce request id on the session control stream.
         * Used after peering fanout when the publisher session is a real MoQ client (non-zero connection handle).
         */
        void SendPublishNamespaceAck(moxygen::RequestID request_id);

        /** Picoquic/proxygen transport used for uni-stream fetch responses (MoQSession `wt_`). */
        proxygen::WebTransport* UnderlyingWebTransport() const noexcept { return wt_.get(); }
    };

} // namespace laps
