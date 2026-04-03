#pragma once

#include <moxygen/relay/MoQRelay.h>

#include <folly/coro/Task.h>

#include <memory>

namespace laps {

    class MoxygenClientManager;

    /**
     * @brief MoQRelay subclass: after a successful subscribe, notify `MoxygenClientManager` so `RelayCore`
     *        can update state and fan out to peers (libquicr parity with `SubscribeReceived` + `ProcessSubscribe`).
     */
    class MoxygenRelayWithPolicy final : public moxygen::MoQRelay
    {
      public:
        MoxygenRelayWithPolicy(MoxygenClientManager& owner, size_t max_cached_tracks, size_t max_cached_groups_per_track);

        folly::coro::Task<moxygen::Publisher::SubscribeResult> subscribe(
          moxygen::SubscribeRequest sub_req,
          std::shared_ptr<moxygen::TrackConsumer> consumer) override;

        moxygen::Subscriber::PublishResult publish(
          moxygen::PublishRequest pub_req,
          std::shared_ptr<moxygen::Subscriber::SubscriptionHandle> handle = nullptr) override;

      private:
        MoxygenClientManager& owner_;
    };

} // namespace laps
