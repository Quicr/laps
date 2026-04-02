#pragma once

#include <moxygen/MoQTypes.h>

#include <quicr/detail/attributes.h>
#include <quicr/track_name.h>

namespace quicr {
    class SubscribeTrackHandler;
}

namespace laps::moq::shim {

    /**
     * @brief Map moxygen SUBSCRIBE request fields to libquicr shapes for `RelayCore::ProcessSubscribe`.
     *        Delivery timeout is often stripped by MoQSession before the publisher callback; timeout is 0 if absent.
     */
    quicr::FullTrackName MoxygenSubscribeToQuicrFullTrackName(const moxygen::FullTrackName& ftn);

    quicr::messages::SubscribeAttributes MoxygenSubscribeToQuicrAttributes(const moxygen::SubscribeRequest& sub);

    /**
     * @brief Build moxygen `SubscribeRequest` for an upstream (announcer) SUBSCRIBE from `quicr::SubscribeTrackHandler`.
     */
    moxygen::SubscribeRequest MoqShimBuildUpstreamSubscribeRequest(const quicr::SubscribeTrackHandler& handler);

    /** @brief Map libquicr full track name to moxygen wire shape (relay publish / namespace). */
    moxygen::FullTrackName QuicrFullTrackNameToMoxygen(const quicr::FullTrackName& ftn);

    /** @brief Map libquicr track namespace to moxygen `TrackNamespace`. */
    moxygen::TrackNamespace QuicrTrackNamespaceToMoxygen(const quicr::TrackNamespace& ns);

    /** @brief Map inbound MoQ PUBLISH to libquicr shapes for laps relay state (`PublishReceived` parity). */
    quicr::messages::PublishAttributes MoqShimMoxygenPublishRequestToQuicrPublishAttributes(
      const moxygen::PublishRequest& pub);

} // namespace laps::moq::shim
