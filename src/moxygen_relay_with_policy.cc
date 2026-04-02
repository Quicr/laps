// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "moxygen_relay_with_policy.h"

#include "moxygen_client_manager.h"

#include <moxygen/MoQSession.h>

#include <quicr/detail/ctrl_message_types.h>

#include <spdlog/spdlog.h>

namespace laps {

    MoxygenRelayWithPolicy::MoxygenRelayWithPolicy(MoxygenClientManager& owner,
                                                   size_t max_cached_tracks,
                                                   size_t max_cached_groups_per_track)
      : MoQRelay(max_cached_tracks, max_cached_groups_per_track)
      , owner_(owner)
    {
    }

    folly::coro::Task<moxygen::Publisher::SubscribeResult> MoxygenRelayWithPolicy::subscribe(
      moxygen::SubscribeRequest sub_req,
      std::shared_ptr<moxygen::TrackConsumer> consumer)
    {
        const moxygen::SubscribeRequest sub_snapshot = sub_req;
        auto result = co_await MoQRelay::subscribe(std::move(sub_req), std::move(consumer));
        if (!result.hasError()) {
            std::optional<quicr::messages::Location> largest;
            const auto& sub_ok = result.value()->subscribeOk();
            if (sub_ok.largest.has_value()) {
                largest = quicr::messages::Location{ .group = sub_ok.largest->group,
                                                     .object = sub_ok.largest->object };
            }
            laps::ConnectionHandle reg_handle = 0;
            if (const auto sess = moxygen::MoQSession::getRequestSession()) {
                (void)owner_.GetMoqSessionRegistry().TryFindHandle(sess, reg_handle);
            }
            if (reg_handle != 0) {
                owner_.moxygen_moq_port_->StoreInboundSubscription(
                  static_cast<quicr::ConnectionHandle>(reg_handle),
                  sub_snapshot.requestID.value,
                  result.value());
            } else {
                SPDLOG_WARN("MoxygenRelayWithPolicy::subscribe: session not in registry; inbound relay publish "
                            "binding unavailable");
            }
            owner_.OnInboundMoqSubscribeOk(sub_snapshot, largest);
        }
        co_return result;
    }

    moxygen::Subscriber::PublishResult MoxygenRelayWithPolicy::publish(
      moxygen::PublishRequest pub_req,
      std::shared_ptr<moxygen::Subscriber::SubscriptionHandle> handle)
    {
        const moxygen::PublishRequest pub_snapshot = pub_req;
        auto result = MoQRelay::publish(std::move(pub_req), std::move(handle));
        if (!result.hasValue()) {
            return result;
        }
        laps::ConnectionHandle reg_handle = 0;
        if (const auto sess = moxygen::MoQSession::getRequestSession()) {
            (void)owner_.GetMoqSessionRegistry().TryFindHandle(sess, reg_handle);
        }
        if (reg_handle != 0) {
            owner_.ProcessMoqIngressPublish(static_cast<quicr::ConnectionHandle>(reg_handle), pub_snapshot);
        }
        return result;
    }

} // namespace laps
