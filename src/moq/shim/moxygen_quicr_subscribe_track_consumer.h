#pragma once

#include <moxygen/MoQConsumers.h>

#include <memory>

namespace quicr {
    class SubscribeTrackHandler;
}

namespace laps::moq::shim {

    /**
     * @brief `TrackConsumer` that forwards moxygen-delivered objects to `quicr::SubscribeTrackHandler`.
     */
    std::shared_ptr<moxygen::TrackConsumer> MakeQuicrSubscribeTrackMoqConsumer(
      std::shared_ptr<quicr::SubscribeTrackHandler> handler);

} // namespace laps::moq::shim
