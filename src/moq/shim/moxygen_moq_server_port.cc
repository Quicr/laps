// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "moxygen_moq_server_port.h"

#include "moxygen_client_manager.h"
#include "moxygen_quicr_subscribe_track_consumer.h"
#include "moxygen_subscribe_to_relay.h"
#include "laps_moq_relay_session.h"
#include "fetch_handler.h"
#include "publish_fetch_handler.h"
#include "publish_namespace_handler.h"
#include "subscribe_handler.h"

#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQPublishError.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/Publisher.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/FutureUtil.h>
#include <folly/coro/Invoke.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <quicr/detail/messages.h>
#include <quicr/fetch_track_handler.h>
#include <quicr/publish_fetch_handler.h>
#include <quicr/publish_namespace_handler.h>
#include <quicr/subscribe_track_handler.h>
#include <quicr/track_name.h>

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>
#include <vector>

namespace laps::moq::shim {

    namespace {

        std::shared_ptr<spdlog::logger> EffectiveLogger(const std::shared_ptr<spdlog::logger>& log)
        {
            return log ? log : spdlog::default_logger();
        }

        proxygen::WebTransport* MoqSessionWebTransport(const std::shared_ptr<moxygen::MoQSession>& session)
        {
            const auto laps = std::dynamic_pointer_cast<laps::LapsMoqRelaySession>(session);
            return laps ? laps->UnderlyingWebTransport() : nullptr;
        }

        moxygen::RequestUpdate MoqShimBuildUpstreamRequestUpdate(const quicr::SubscribeTrackHandler& handler,
                                                                 const std::optional<uint64_t>& pending_new_group_id)
        {
            moxygen::RequestUpdate ru;
            ru.requestID = moxygen::RequestID{ 0 };
            ru.existingRequestID = moxygen::RequestID{ 0 };
            ru.priority = handler.GetPriority();
            ru.forward = std::optional<bool>{ true };
            if (const auto dt = handler.GetDeliveryTimeout(); dt.has_value() && dt->count() > 0) {
                (void)ru.params.insertParam(moxygen::Parameter(
                  static_cast<uint64_t>(moxygen::TrackRequestParamKey::DELIVERY_TIMEOUT),
                  static_cast<uint64_t>(dt->count())));
            }
            if (pending_new_group_id.has_value()) {
                (void)ru.params.insertParam(moxygen::Parameter(
                  static_cast<uint64_t>(moxygen::TrackRequestParamKey::NEW_GROUP_REQUEST),
                  *pending_new_group_id));
            }
            return ru;
        }

        moxygen::PublishRequest MoqShimBuildRelayPublishRequestForHandler(const quicr::PublishTrackHandler& handler)
        {
            moxygen::PublishRequest pub;
            pub.requestID = moxygen::RequestID{ 0 };
            pub.fullTrackName = QuicrFullTrackNameToMoxygen(handler.GetFullTrackName());
            pub.forward = true;
            pub.groupOrder = moxygen::GroupOrder::Default;
            const uint32_t ttl = handler.GetDefaultTTL();
            if (ttl > 0) {
                (void)pub.params.insertParam(moxygen::Parameter(
                  static_cast<uint64_t>(moxygen::TrackRequestParamKey::DELIVERY_TIMEOUT),
                  static_cast<uint64_t>(ttl)));
            }
            return pub;
        }

        moxygen::ObjectStatus QuicrObjectStatusToMoq(quicr::ObjectStatus s)
        {
            switch (s) {
            case quicr::ObjectStatus::kAvailable:
                return moxygen::ObjectStatus::NORMAL;
            case quicr::ObjectStatus::kDoesNotExist:
                return moxygen::ObjectStatus::OBJECT_NOT_EXIST;
            case quicr::ObjectStatus::kEndOfGroup:
                return moxygen::ObjectStatus::END_OF_GROUP;
            case quicr::ObjectStatus::kEndOfTrack:
                return moxygen::ObjectStatus::END_OF_TRACK;
            case quicr::ObjectStatus::kEndOfSubGroup:
            default:
                return moxygen::ObjectStatus::END_OF_GROUP;
            }
        }

        constexpr uint64_t kQuicrFetchBridgeStreamId = 0;

        moxygen::GroupOrder QuicrGroupOrderToMoqFetch(quicr::messages::GroupOrder go)
        {
            switch (go) {
            case quicr::messages::GroupOrder::kOriginalPublisherOrder:
                return moxygen::GroupOrder::Default;
            case quicr::messages::GroupOrder::kAscending:
                return moxygen::GroupOrder::OldestFirst;
            case quicr::messages::GroupOrder::kDescending:
                return moxygen::GroupOrder::NewestFirst;
            default:
                return moxygen::GroupOrder::Default;
            }
        }

        void AppendIOBufChainToVector(const folly::IOBuf* chain, std::vector<uint8_t>& dest)
        {
            if (!chain || chain->empty()) {
                return;
            }
            const std::size_t len = chain->computeChainDataLength();
            const std::size_t old = dest.size();
            dest.resize(old + len);
            folly::io::Cursor cursor(chain);
            cursor.pull(dest.data() + old, len);
        }

        /**
         * Converts MoQ fetch stream delivery into libquicr fetch wire chunks for `FetchTrackHandler::StreamDataRecv`.
         */
        class MoqFetchToQuicrBridge final : public moxygen::FetchConsumer
        {
          public:
            MoqFetchToQuicrBridge(std::weak_ptr<quicr::FetchTrackHandler> handler, std::shared_ptr<spdlog::logger> log)
              : handler_(std::move(handler))
              , log_(std::move(log))
            {
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
              uint64_t groupID,
              uint64_t subgroupID,
              uint64_t objectID,
              moxygen::Payload payload,
              moxygen::Extensions /*extensions*/,
              bool /*finFetch*/,
              bool /*forwardingPreferenceIsDatagram*/) override
            {
                if (partial_) {
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::API_ERROR, "fetch bridge: object while partial in progress"));
                }
                std::vector<uint8_t> bytes;
                AppendIOBufChainToVector(payload ? payload.get() : nullptr, bytes);
                quicr::messages::FetchObject fo{};
                fo.group_id = groupID;
                fo.subgroup_id = subgroupID;
                fo.object_id = objectID;
                if (const auto th = handler_.lock()) {
                    fo.publisher_priority = th->GetPriority();
                }
                fo.payload = std::move(bytes);
                fo.object_status = quicr::ObjectStatus::kAvailable;
                sendOneFetchObject(fo);
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
              uint64_t groupID,
              uint64_t subgroupID,
              uint64_t objectID,
              uint64_t length,
              moxygen::Payload initialPayload,
              moxygen::Extensions /*extensions*/) override
            {
                if (partial_) {
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::API_ERROR, "fetch bridge: nested beginObject"));
                }
                PartialObject p{};
                p.group_id = groupID;
                p.subgroup_id = subgroupID;
                p.object_id = objectID;
                p.expected_len = length;
                AppendIOBufChainToVector(initialPayload ? initialPayload.get() : nullptr, p.buf);
                partial_ = std::move(p);
                return folly::unit;
            }

            folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError> objectPayload(
              moxygen::Payload payload,
              bool /*finSubgroup*/) override
            {
                if (!partial_) {
                    return folly::makeUnexpected(
                      moxygen::MoQPublishError(moxygen::MoQPublishError::API_ERROR, "fetch bridge: objectPayload"));
                }
                AppendIOBufChainToVector(payload ? payload.get() : nullptr, partial_->buf);
                if (partial_->buf.size() > partial_->expected_len) {
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::API_ERROR, "fetch bridge: object size overflow"));
                }
                if (partial_->buf.size() == partial_->expected_len) {
                    quicr::messages::FetchObject fo{};
                    fo.group_id = partial_->group_id;
                    fo.subgroup_id = partial_->subgroup_id;
                    fo.object_id = partial_->object_id;
                    if (const auto th = handler_.lock()) {
                        fo.publisher_priority = th->GetPriority();
                    }
                    fo.payload = std::move(partial_->buf);
                    fo.object_status = quicr::ObjectStatus::kAvailable;
                    partial_.reset();
                    sendOneFetchObject(fo);
                    return moxygen::ObjectPublishStatus::DONE;
                }
                return moxygen::ObjectPublishStatus::IN_PROGRESS;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(
              uint64_t groupID,
              uint64_t subgroupID,
              uint64_t objectID,
              bool /*finFetch*/) override
            {
                quicr::messages::FetchObject fo{};
                fo.group_id = groupID;
                fo.subgroup_id = subgroupID;
                fo.object_id = objectID;
                if (const auto th = handler_.lock()) {
                    fo.publisher_priority = th->GetPriority();
                }
                fo.payload.clear();
                fo.object_status = quicr::ObjectStatus::kEndOfGroup;
                sendOneFetchObject(fo);
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfTrackAndGroup(
              uint64_t groupID,
              uint64_t subgroupID,
              uint64_t objectID) override
            {
                quicr::messages::FetchObject fo{};
                fo.group_id = groupID;
                fo.subgroup_id = subgroupID;
                fo.object_id = objectID;
                if (const auto th = handler_.lock()) {
                    fo.publisher_priority = th->GetPriority();
                }
                fo.payload.clear();
                fo.object_status = quicr::ObjectStatus::kEndOfTrack;
                sendOneFetchObject(fo);
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfFetch() override { return folly::unit; }

            void reset(moxygen::ResetStreamErrorCode /*error*/) override {}

            folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError> awaitReadyToConsume() override
            {
                return folly::makeSemiFuture<uint64_t>(0);
            }

          private:
            struct PartialObject
            {
                uint64_t group_id{};
                uint64_t subgroup_id{};
                uint64_t object_id{};
                uint64_t expected_len{};
                std::vector<uint8_t> buf;
            };

            void sendOneFetchObject(const quicr::messages::FetchObject& obj)
            {
                quicr::Bytes wire;
                if (!header_sent_) {
                    quicr::messages::FetchHeader hdr;
                    hdr.request_id = 0;
                    wire << hdr;
                    header_sent_ = true;
                }
                wire << obj;
                const auto th = handler_.lock();
                if (!th) {
                    return;
                }
                try {
                    auto vec = std::make_shared<std::vector<uint8_t>>(wire.begin(), wire.end());
                    th->StreamDataRecv(first_quic_chunk_, kQuicrFetchBridgeStreamId, std::move(vec));
                    first_quic_chunk_ = false;
                } catch (const std::exception& ex) {
                    SPDLOG_LOGGER_WARN(log_, "MoqFetchToQuicrBridge: StreamDataRecv exception: {}", ex.what());
                }
            }

            std::weak_ptr<quicr::FetchTrackHandler> handler_;
            std::shared_ptr<spdlog::logger> log_;
            bool header_sent_{ false };
            bool first_quic_chunk_{ true };
            std::optional<PartialObject> partial_;
        };

        quicr::PublishTrackHandler::PublishObjectStatus WriteMoqFetchResponseChunk(
          proxygen::WebTransport::StreamWriteHandle* wh,
          quicr::BytesSpan span,
          bool fin)
        {
            if (!wh) {
                return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
            }
            if (span.empty() && !fin) {
                return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
            }
            std::unique_ptr<folly::IOBuf> buf =
              span.empty() ? folly::IOBuf::create(0) : folly::IOBuf::copyBuffer(span.data(), span.size());
            const auto r = wh->writeStreamData(std::move(buf), fin, nullptr);
            if (!r.hasValue()) {
                return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
            }
            return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
        }

    } // namespace

    MoxygenMoqServerPort::MoxygenMoqServerPort(MoxygenClientManager& owner, std::shared_ptr<spdlog::logger> log)
      : owner_(owner)
      , log_(EffectiveLogger(log))
    {
    }

    MoxygenMoqServerPort::~MoxygenMoqServerPort() = default;

    std::shared_ptr<moxygen::MoQSession> MoxygenMoqServerPort::LockMoqSession(
      quicr::ConnectionHandle connection_handle) const
    {
        return owner_.GetMoqSessionRegistry().LockSession(static_cast<laps::ConnectionHandle>(connection_handle));
    }

    void MoxygenMoqServerPort::SubscribeTrack(quicr::ConnectionHandle connection_handle,
                                              std::shared_ptr<quicr::SubscribeTrackHandler> track_handler)
    {
        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::SubscribeTrack: no MoQSession for connection_handle={}",
                               connection_handle);
            return;
        }
        if (!track_handler) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::SubscribeTrack: null handler connection_handle={}",
                               connection_handle);
            return;
        }
        if (!session->getExecutor()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::SubscribeTrack: session has no executor ch={}",
                               connection_handle);
            return;
        }

        moxygen::SubscribeRequest sub_req = MoqShimBuildUpstreamSubscribeRequest(*track_handler);
        auto consumer = MakeQuicrSubscribeTrackMoqConsumer(track_handler);

        try {
            folly::coro::blockingWait(folly::coro::co_withExecutor(
              session->getExecutor(),
                folly::coro::co_invoke(
                [this, session, track_handler, connection_handle, log = log_, sub_req = std::move(sub_req),
                 consumer = std::move(consumer)]() mutable -> folly::coro::Task<void> {
                    auto result = co_await folly::coro::co_awaitTry(
                      session->subscribe(std::move(sub_req), std::move(consumer)));
                    if (result.hasException()) {
                        const char* const ex_what = result.exception().what().c_str();
                        SPDLOG_LOGGER_WARN(log,
                                           "MoxygenMoqServerPort::SubscribeTrack: subscribe exception ch={} ex={}",
                                           connection_handle,
                                           ex_what ? ex_what : "");
                        co_return;
                    }
                    auto& subscribe_result = result.value();
                    if (!subscribe_result.hasValue()) {
                        const auto& err = subscribe_result.error();
                        SPDLOG_LOGGER_WARN(log,
                                           "MoxygenMoqServerPort::SubscribeTrack: SUBSCRIBE_ERROR ch={} code={} "
                                           "reason={}",
                                           connection_handle,
                                           static_cast<unsigned>(err.errorCode),
                                           std::string(err.reasonPhrase));
                        co_return;
                    }
                    const auto moq_sub_handle = subscribe_result.value();
                    const moxygen::SubscribeOk& ok = moq_sub_handle->subscribeOk();
                    this->StoreUpstreamSubscription({ connection_handle, ok.requestID.value }, moq_sub_handle);
                    if (auto laps_sub = std::dynamic_pointer_cast<laps::SubscribeTrackHandler>(track_handler)) {
                        laps_sub->SetRelayUpstreamConnection(connection_handle);
                    }
                    track_handler->SetRequestId(ok.requestID.value);
                    track_handler->SetReceivedTrackAlias(ok.trackAlias.value);
                    track_handler->SetTrackAlias(ok.trackAlias.value);
                    if (ok.largest.has_value()) {
                        track_handler->SetLatestLocation(
                          quicr::messages::Location{ ok.largest->group, ok.largest->object });
                    }
                    SPDLOG_LOGGER_INFO(log,
                                       "MoxygenMoqServerPort::SubscribeTrack: ok ch={} req_id={} track_alias={}",
                                       connection_handle,
                                       ok.requestID.value,
                                       ok.trackAlias.value);
                })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::SubscribeTrack: blockingWait failed ch={} ex={}",
                               connection_handle,
                               ex.what());
        }
    }

    void MoxygenMoqServerPort::UnsubscribeTrack(
      quicr::ConnectionHandle connection_handle,
      const std::shared_ptr<quicr::SubscribeTrackHandler>& track_handler)
    {
        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UnsubscribeTrack: no MoQSession for connection_handle={}",
                               connection_handle);
            return;
        }
        if (!track_handler || !track_handler->GetRequestId().has_value()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UnsubscribeTrack: missing handler or request_id ch={}",
                               connection_handle);
            return;
        }
        auto moq_handle = TakeUpstreamSubscription({ connection_handle, *track_handler->GetRequestId() });
        if (!moq_handle) {
            SPDLOG_LOGGER_DEBUG(log_,
                                "MoxygenMoqServerPort::UnsubscribeTrack: no cached upstream handle ch={} req_id={}",
                                connection_handle,
                                *track_handler->GetRequestId());
            return;
        }
        if (!session->getExecutor()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UnsubscribeTrack: session has no executor ch={}",
                               connection_handle);
            return;
        }
        try {
            folly::coro::blockingWait(folly::coro::co_withExecutor(
              session->getExecutor(),
              folly::coro::co_invoke([h = std::move(moq_handle)]() mutable -> folly::coro::Task<void> {
                  h->unsubscribe();
                  co_return;
              })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UnsubscribeTrack: blockingWait failed ch={} ex={}",
                               connection_handle,
                               ex.what());
        }
    }

    void MoxygenMoqServerPort::UpdateTrackSubscription(
      quicr::ConnectionHandle connection_handle,
      std::shared_ptr<quicr::SubscribeTrackHandler> track_handler)
    {
        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UpdateTrackSubscription: no MoQSession for "
                               "connection_handle={}",
                               connection_handle);
            return;
        }
        if (!track_handler || !track_handler->GetRequestId().has_value()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UpdateTrackSubscription: missing handler or request_id ch={}",
                               connection_handle);
            return;
        }
        const auto moq_handle = FindUpstreamSubscription({ connection_handle, *track_handler->GetRequestId() });
        if (!moq_handle) {
            SPDLOG_LOGGER_DEBUG(log_,
                                "MoxygenMoqServerPort::UpdateTrackSubscription: no cached upstream handle ch={} "
                                "req_id={}",
                                connection_handle,
                                *track_handler->GetRequestId());
            return;
        }
        if (!session->getExecutor()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UpdateTrackSubscription: session has no executor ch={}",
                               connection_handle);
            return;
        }
        std::optional<uint64_t> pending_ngr;
        if (auto laps_sub = std::dynamic_pointer_cast<laps::SubscribeTrackHandler>(track_handler)) {
            pending_ngr = laps_sub->GetPendingNewRquestId();
        }
        moxygen::RequestUpdate ru = MoqShimBuildUpstreamRequestUpdate(*track_handler, pending_ngr);
        try {
            folly::coro::blockingWait(folly::coro::co_withExecutor(
              session->getExecutor(),
              folly::coro::co_invoke([moq_handle, ru = std::move(ru), log = log_, track_handler]() mutable
                                       -> folly::coro::Task<void> {
                                           auto update_result = co_await moq_handle->requestUpdate(std::move(ru));
                                           if (update_result.hasError()) {
                                               const auto& err = update_result.error();
                                               SPDLOG_LOGGER_WARN(log,
                                                                  "MoxygenMoqServerPort::UpdateTrackSubscription: "
                                                                  "REQUEST_ERROR code={} reason={}",
                                                                  static_cast<unsigned>(err.errorCode),
                                                                  err.reasonPhrase);
                                           } else if (auto laps_sub =
                                                        std::dynamic_pointer_cast<laps::SubscribeTrackHandler>(
                                                          track_handler)) {
                                               laps_sub->ClearPendingNewGroupRequestId();
                                           }
                                           co_return;
                                       })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::UpdateTrackSubscription: blockingWait failed ch={} ex={}",
                               connection_handle,
                               ex.what());
        }
    }

    void MoxygenMoqServerPort::PublishNamespace(quicr::ConnectionHandle connection_handle,
                                                std::shared_ptr<quicr::PublishNamespaceHandler> ns_handler,
                                                bool passive)
    {
        if (!ns_handler) {
            SPDLOG_LOGGER_WARN(log_, "MoxygenMoqServerPort::PublishNamespace: null handler ch={}", connection_handle);
            return;
        }
        const auto laps_ns = std::dynamic_pointer_cast<laps::PublishNamespaceHandler>(ns_handler);
        if (!laps_ns) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::PublishNamespace: expected laps::PublishNamespaceHandler ch={}",
                               connection_handle);
            return;
        }
        laps_ns->SetWireConnectionId(connection_handle);

        if (passive) {
            laps_ns->CompleteMoqPublishNamespace(true);
            return;
        }

        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::PublishNamespace: no MoQSession for connection_handle={}",
                               connection_handle);
            laps_ns->CompleteMoqPublishNamespace(
              false,
              std::make_optional(
                std::make_pair(quicr::messages::ErrorCode::kInternalError, std::string{ "no MoQ session" })));
            return;
        }

        const auto relay = std::dynamic_pointer_cast<moxygen::MoQRelaySession>(session);
        if (!relay) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::PublishNamespace: session is not MoQRelaySession ch={}",
                               connection_handle);
            laps_ns->CompleteMoqPublishNamespace(
              false,
              std::make_optional(std::make_pair(quicr::messages::ErrorCode::kInternalError,
                                                std::string{ "MoQRelaySession required" })));
            return;
        }

        if (!relay->getExecutor()) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::PublishNamespace: no executor ch={}",
                               connection_handle);
            laps_ns->CompleteMoqPublishNamespace(
              false,
              std::make_optional(
                std::make_pair(quicr::messages::ErrorCode::kInternalError, std::string{ "no executor" })));
            return;
        }

        moxygen::PublishNamespace ann;
        ann.trackNamespace = QuicrTrackNamespaceToMoxygen(ns_handler->GetPrefix());
        ann.requestID = moxygen::RequestID{ 0 };

        const auto ns_hash =
          quicr::TrackHash(quicr::FullTrackName{ ns_handler->GetPrefix(), {} }).track_namespace_hash;

        try {
            const auto result = folly::coro::blockingWait(folly::coro::co_withExecutor(
              relay->getExecutor(),
              folly::coro::co_invoke([relay, ann = std::move(ann)]() mutable
                                     -> folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult> {
                  co_return co_await relay->publishNamespace(std::move(ann), nullptr);
              })));

            if (result.hasError()) {
                SPDLOG_LOGGER_WARN(log_,
                                   "MoxygenMoqServerPort::PublishNamespace: error ch={} ns_hash={} reason={}",
                                   connection_handle,
                                   ns_hash,
                                   std::string(result.error().reasonPhrase));
                laps_ns->CompleteMoqPublishNamespace(
                  false,
                  std::make_optional(std::make_pair(quicr::messages::ErrorCode::kInternalError,
                                                    std::string(result.error().reasonPhrase))));
                return;
            }

            const auto& ann_ok = result.value()->publishNamespaceOk();
            ns_handler->SetRequestId(ann_ok.requestID.value);
            laps_ns->CompleteMoqPublishNamespace(true);
            SPDLOG_LOGGER_INFO(log_,
                               "MoxygenMoqServerPort::PublishNamespace: ok ch={} ns_hash={} req_id={}",
                               connection_handle,
                               ns_hash,
                               ann_ok.requestID.value);
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::PublishNamespace: exception ch={} ex={}",
                               connection_handle,
                               ex.what());
            laps_ns->CompleteMoqPublishNamespace(
              false,
              std::make_optional(
                std::make_pair(quicr::messages::ErrorCode::kInternalError, std::string(ex.what()))));
        }
    }

    void MoxygenMoqServerPort::FanoutPublishNamespaceAnnounces(
      quicr::ConnectionHandle publisher_connection_handle,
      const quicr::TrackNamespace& track_namespace,
      quicr::messages::RequestID publisher_request_id,
      const std::vector<quicr::ConnectionHandle>& subscriber_connections)
    {
        const auto log = EffectiveLogger(log_);
        const auto moq_ns = QuicrTrackNamespaceToMoxygen(track_namespace);
        const auto th = quicr::TrackHash(quicr::FullTrackName{ track_namespace, {} });

        if (publisher_connection_handle != 0) {
            const auto pub_session = LockMoqSession(publisher_connection_handle);
            const auto pub_laps = std::dynamic_pointer_cast<LapsMoqRelaySession>(pub_session);
            if (pub_laps && pub_laps->getExecutor()) {
                const auto pr = moxygen::RequestID{ publisher_request_id };
                try {
                    folly::coro::blockingWait(folly::coro::co_withExecutor(
                      pub_laps->getExecutor(),
                      folly::coro::co_invoke([pub_laps, pr]() mutable -> folly::coro::Task<void> {
                          pub_laps->SendPublishNamespaceAck(pr);
                          co_return;
                      })));
                    SPDLOG_LOGGER_DEBUG(log,
                                        "FanoutPublishNamespaceAnnounces: publisher PUBLISH_NAMESPACE_OK ch={} "
                                        "req_id={}",
                                        publisher_connection_handle,
                                        publisher_request_id);
                } catch (const std::exception& ex) {
                    SPDLOG_LOGGER_WARN(log,
                                       "FanoutPublishNamespaceAnnounces: publisher ack failed ch={} ex={}",
                                       publisher_connection_handle,
                                       ex.what());
                }
            } else {
                SPDLOG_LOGGER_WARN(log,
                                   "FanoutPublishNamespaceAnnounces: no LapsMoqRelaySession for publisher ch={}",
                                   publisher_connection_handle);
            }
        }

        for (const auto sub_ch : subscriber_connections) {
            if (sub_ch == 0) {
                continue;
            }

            {
                std::lock_guard<std::mutex> fanout_lock(peer_fanout_ns_mu_);
                peer_fanout_publish_namespace_handles_.erase({ sub_ch, publisher_request_id });
            }

            const auto session = LockMoqSession(sub_ch);
            const auto relay = std::dynamic_pointer_cast<moxygen::MoQRelaySession>(session);
            if (!relay || !relay->getExecutor()) {
                SPDLOG_LOGGER_WARN(log,
                                   "FanoutPublishNamespaceAnnounces: skip subscriber ch={} (no MoQRelaySession or "
                                   "executor) ns_hash={}",
                                   sub_ch,
                                   th.track_namespace_hash);
                continue;
            }

            moxygen::PublishNamespace ann;
            ann.trackNamespace = moq_ns;
            ann.requestID = moxygen::RequestID{ 0 };

            try {
                const auto result = folly::coro::blockingWait(folly::coro::co_withExecutor(
                  relay->getExecutor(),
                  folly::coro::co_invoke([relay, ann = std::move(ann)]() mutable
                                           -> folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult> {
                      co_return co_await relay->publishNamespace(std::move(ann), nullptr);
                  })));

                if (result.hasError()) {
                    SPDLOG_LOGGER_WARN(log,
                                       "FanoutPublishNamespaceAnnounces: publishNamespace failed sub_ch={} "
                                       "ns_hash={} reason={}",
                                       sub_ch,
                                       th.track_namespace_hash,
                                       std::string(result.error().reasonPhrase));
                    continue;
                }
                std::lock_guard<std::mutex> fanout_lock(peer_fanout_ns_mu_);
                peer_fanout_publish_namespace_handles_[{ sub_ch, publisher_request_id }] = result.value();
                SPDLOG_LOGGER_DEBUG(log,
                                    "FanoutPublishNamespaceAnnounces: stored handle sub_ch={} publisher_req_id={} "
                                    "ns_hash={}",
                                    sub_ch,
                                    publisher_request_id,
                                    th.track_namespace_hash);
            } catch (const std::exception& ex) {
                SPDLOG_LOGGER_WARN(
                  log, "FanoutPublishNamespaceAnnounces: exception sub_ch={} ex={}", sub_ch, ex.what());
            }
        }
    }

    void MoxygenMoqServerPort::FanoutPublishNamespaceDones(
      quicr::messages::RequestID publisher_request_id,
      const std::vector<quicr::ConnectionHandle>& subscriber_connections)
    {
        const auto log = EffectiveLogger(log_);
        for (const auto sub_ch : subscriber_connections) {
            if (sub_ch == 0) {
                continue;
            }
            std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> handle;
            {
                std::lock_guard<std::mutex> fanout_lock(peer_fanout_ns_mu_);
                const auto it = peer_fanout_publish_namespace_handles_.find({ sub_ch, publisher_request_id });
                if (it == peer_fanout_publish_namespace_handles_.end()) {
                    SPDLOG_LOGGER_DEBUG(log,
                                        "FanoutPublishNamespaceDones: no fanout handle sub_ch={} req_id={}",
                                        sub_ch,
                                        publisher_request_id);
                    continue;
                }
                handle = std::move(it->second);
                peer_fanout_publish_namespace_handles_.erase(it);
            }
            if (handle) {
                handle->publishNamespaceDone();
            }
        }
    }

    void MoxygenMoqServerPort::ClearPeerFanoutPublishNamespaceHandlesForSubscriber(
      quicr::ConnectionHandle subscriber_connection_handle)
    {
        const auto log = EffectiveLogger(log_);
        std::vector<std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle>> pending_done;
        {
            std::lock_guard<std::mutex> fanout_lock(peer_fanout_ns_mu_);
            for (auto it = peer_fanout_publish_namespace_handles_.begin();
                 it != peer_fanout_publish_namespace_handles_.end();) {
                if (it->first.first == subscriber_connection_handle) {
                    pending_done.push_back(std::move(it->second));
                    it = peer_fanout_publish_namespace_handles_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& h : pending_done) {
            if (h) {
                h->publishNamespaceDone();
            }
        }
        if (!pending_done.empty()) {
            SPDLOG_LOGGER_DEBUG(log,
                                "ClearPeerFanoutPublishNamespaceHandlesForSubscriber: cleared {} handles for ch={}",
                                pending_done.size(),
                                subscriber_connection_handle);
        }
    }

    void MoxygenMoqServerPort::BindFetchTrack(quicr::ConnectionHandle connection_handle,
                                              std::shared_ptr<quicr::PublishFetchHandler> track_handler)
    {
        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::BindFetchTrack: no MoQSession for connection_handle={}",
                               connection_handle);
            return;
        }
        if (!track_handler || !track_handler->GetRequestId().has_value()) {
            SPDLOG_LOGGER_WARN(log_, "MoxygenMoqServerPort::BindFetchTrack: invalid handler ch={}", connection_handle);
            return;
        }
        if (const auto laps_pf = std::dynamic_pointer_cast<laps::PublishFetchHandler>(track_handler)) {
            const uint64_t dcid = next_moq_fetch_data_ctx_.fetch_add(1, std::memory_order_relaxed);
            laps_pf->BindForMoxygenFetch(connection_handle, dcid);
        }
        std::lock_guard<std::mutex> lock(fetch_state_mu_);
        fetch_publish_bindings_[std::make_pair(connection_handle, *track_handler->GetRequestId())] = track_handler;
        SPDLOG_LOGGER_DEBUG(log_,
                            "MoxygenMoqServerPort::BindFetchTrack: ch={} req_id={}",
                            connection_handle,
                            *track_handler->GetRequestId());
    }

    void MoxygenMoqServerPort::UnbindFetchTrack(
      quicr::ConnectionHandle connection_handle,
      const std::shared_ptr<quicr::PublishFetchHandler>& track_handler)
    {
        (void)LockMoqSession(connection_handle);
        if (!track_handler || !track_handler->GetRequestId().has_value()) {
            return;
        }
        std::lock_guard<std::mutex> lock(fetch_state_mu_);
        RemoveFetchResponseStreamLocked(track_handler.get());
        fetch_publish_bindings_.erase(std::make_pair(connection_handle, *track_handler->GetRequestId()));
        SPDLOG_LOGGER_DEBUG(log_,
                            "MoxygenMoqServerPort::UnbindFetchTrack: ch={} req_id={}",
                            connection_handle,
                            *track_handler->GetRequestId());
    }

    void MoxygenMoqServerPort::FetchTrack(quicr::ConnectionHandle connection_handle,
                                          std::shared_ptr<quicr::FetchTrackHandler> track_handler)
    {
        const auto session = LockMoqSession(connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::FetchTrack: no MoQSession for connection_handle={}",
                               connection_handle);
            return;
        }
        if (!track_handler) {
            SPDLOG_LOGGER_WARN(log_, "MoxygenMoqServerPort::FetchTrack: null handler ch={}", connection_handle);
            return;
        }
        if (!session->getExecutor()) {
            SPDLOG_LOGGER_WARN(log_, "MoxygenMoqServerPort::FetchTrack: no executor ch={}", connection_handle);
            return;
        }

        const auto laps_fetch = std::dynamic_pointer_cast<laps::FetchTrackHandler>(track_handler);
        if (!laps_fetch) {
            SPDLOG_LOGGER_WARN(log_,
                               "MoxygenMoqServerPort::FetchTrack: expected laps::FetchTrackHandler ch={}",
                               connection_handle);
            return;
        }

        const auto th = quicr::TrackHash(track_handler->GetFullTrackName());
        if (!track_handler->GetTrackAlias().has_value()) {
            track_handler->SetTrackAlias(th.track_fullname_hash);
        }

        const auto fetch_key =
          std::make_pair(connection_handle, reinterpret_cast<uintptr_t>(track_handler.get()));
        {
            std::lock_guard<std::mutex> lk(fetch_state_mu_);
            if (const auto it = active_outbound_fetches_.find(fetch_key); it != active_outbound_fetches_.end()) {
                it->second->fetchCancel();
                active_outbound_fetches_.erase(it);
            }
        }

        const auto start = track_handler->GetStartLocation();
        const auto end = track_handler->GetEndLocation();
        moxygen::Fetch moq_fetch(
          moxygen::RequestID{ 0 },
          QuicrFullTrackNameToMoxygen(track_handler->GetFullTrackName()),
          moxygen::AbsoluteLocation{ start.group, start.object },
          moxygen::AbsoluteLocation{ end.group, end.object.value_or(0) },
          track_handler->GetPriority(),
          QuicrGroupOrderToMoqFetch(track_handler->GetGroupOrder()),
          std::vector<moxygen::Parameter>{});

        auto consumer =
          std::make_shared<MoqFetchToQuicrBridge>(track_handler, log_);

        try {
            folly::coro::blockingWait(folly::coro::co_withExecutor(
              session->getExecutor(),
              folly::coro::co_invoke([this,
                                      session,
                                      connection_handle,
                                      track_handler,
                                      laps_fetch,
                                      moq_fetch = std::move(moq_fetch),
                                      consumer = std::move(consumer),
                                      fetch_key,
                                      log = log_]() mutable -> folly::coro::Task<void> {
                  laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kPendingResponse);
                  auto tried = co_await folly::coro::co_awaitTry(session->fetch(std::move(moq_fetch), std::move(consumer)));
                  if (tried.hasException()) {
                      const char* const w = tried.exception().what().c_str();
                      SPDLOG_LOGGER_WARN(log,
                                         "MoxygenMoqServerPort::FetchTrack: fetch exception ch={} ex={}",
                                         connection_handle,
                                         w ? w : "");
                      laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kError);
                      co_return;
                  }
                  auto& fetch_result = tried.value();
                  if (fetch_result.hasError()) {
                      SPDLOG_LOGGER_WARN(log,
                                         "MoxygenMoqServerPort::FetchTrack: FETCH_ERROR ch={} reason={}",
                                         connection_handle,
                                         std::string(fetch_result.error().reasonPhrase));
                      laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kError);
                      co_return;
                  }
                  auto moq_handle = std::move(fetch_result.value());
                  const auto& ok = moq_handle->fetchOk();
                  track_handler->SetRequestId(ok.requestID.value);
                  track_handler->SetLatestLocation(
                    quicr::messages::Location{ ok.endLocation.group, ok.endLocation.object });
                  laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kOk);
                  {
                      std::lock_guard<std::mutex> lk(fetch_state_mu_);
                      active_outbound_fetches_[fetch_key] = std::move(moq_handle);
                  }
                  SPDLOG_LOGGER_DEBUG(log,
                                      "MoxygenMoqServerPort::FetchTrack: ok ch={} moq_req_id={}",
                                      connection_handle,
                                      ok.requestID.value);
              })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(
              log_, "MoxygenMoqServerPort::FetchTrack: blockingWait failed ch={} ex={}", connection_handle, ex.what());
            if (laps_fetch) {
                laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kError);
            }
        }
    }

    void MoxygenMoqServerPort::CancelFetchTrack(quicr::ConnectionHandle connection_handle,
                                                std::shared_ptr<quicr::FetchTrackHandler> track_handler)
    {
        if (!track_handler) {
            return;
        }
        const auto laps_fetch = std::dynamic_pointer_cast<laps::FetchTrackHandler>(track_handler);
        (void)LockMoqSession(connection_handle);
        const auto fetch_key =
          std::make_pair(connection_handle, reinterpret_cast<uintptr_t>(track_handler.get()));
        std::shared_ptr<moxygen::Publisher::FetchHandle> handle_copy;
        {
            std::lock_guard<std::mutex> lk(fetch_state_mu_);
            if (const auto it = active_outbound_fetches_.find(fetch_key); it != active_outbound_fetches_.end()) {
                handle_copy = it->second;
                active_outbound_fetches_.erase(it);
            }
        }
        if (handle_copy) {
            handle_copy->fetchCancel();
        }
        track_handler->SetRequestId(std::nullopt);
        if (laps_fetch && track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kDoneByFin &&
            track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kDoneByReset) {
            laps_fetch->ApplyMoqFetchSubscribeStatus(quicr::FetchTrackHandler::Status::kNotConnected);
        }
        SPDLOG_LOGGER_DEBUG(log_, "MoxygenMoqServerPort::CancelFetchTrack: ch={}", connection_handle);
    }

    void MoxygenMoqServerPort::ClearOutboundFetchStateFor(quicr::ConnectionHandle connection_handle)
    {
        std::vector<std::shared_ptr<moxygen::Publisher::FetchHandle>> cancel_list;
        {
            std::lock_guard<std::mutex> lk(fetch_state_mu_);
            for (auto it = active_outbound_fetches_.begin(); it != active_outbound_fetches_.end();) {
                if (it->first.first == connection_handle) {
                    cancel_list.push_back(it->second);
                    it = active_outbound_fetches_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = fetch_publish_bindings_.begin(); it != fetch_publish_bindings_.end();) {
                if (it->first.first == connection_handle) {
                    it = fetch_publish_bindings_.erase(it);
                } else {
                    ++it;
                }
            }
            ClearFetchResponseStreamsForConnectionLocked(connection_handle);
        }
        for (const auto& h : cancel_list) {
            if (h) {
                h->fetchCancel();
            }
        }
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus> MoxygenMoqServerPort::TryMoxygenFetchPublishObject(
      quicr::PublishFetchHandler* handler,
      const quicr::ObjectHeaders& object_headers,
      quicr::BytesSpan data)
    {
        if (!handler) {
            return std::nullopt;
        }
        const auto* laps_pf = dynamic_cast<const laps::PublishFetchHandler*>(handler);
        if (!laps_pf || !laps_pf->IsMoqFetchPortBound()) {
            return std::nullopt;
        }
        if (!handler->GetRequestId().has_value()) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kNoSubscribers;
        }
        const auto conn = handler->GetConnectionId();
        const auto session = LockMoqSession(conn);
        if (!session) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        if (!MoqSessionWebTransport(session)) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        const auto relay = std::dynamic_pointer_cast<laps::LapsMoqRelaySession>(session);
        if (!relay || !relay->getExecutor()) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        std::vector<uint8_t> data_copy(data.begin(), data.end());
        try {
            return folly::coro::blockingWait(folly::coro::co_withExecutor(
              relay->getExecutor(),
              folly::coro::co_invoke(
                [this, handler, object_headers, data_copy = std::move(data_copy)]() mutable
                  -> folly::coro::Task<std::optional<quicr::PublishTrackHandler::PublishObjectStatus>> {
                    co_return TryMoxygenFetchPublishObjectOnExecutor(
                      handler, object_headers, quicr::BytesSpan{ data_copy.data(), data_copy.size() });
                })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_, "TryMoxygenFetchPublishObject: executor error ch={} ex={}", conn, ex.what());
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
    MoxygenMoqServerPort::TryMoxygenFetchPublishObjectOnExecutor(quicr::PublishFetchHandler* handler,
                                                                 const quicr::ObjectHeaders& object_headers,
                                                                 quicr::BytesSpan data)
    {
        const auto req_id = handler->GetRequestId();
        if (!req_id.has_value()) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kNoSubscribers;
        }
        const auto conn = handler->GetConnectionId();
        const auto session = LockMoqSession(conn);
        if (!session) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        auto* const wt = MoqSessionWebTransport(session);
        if (!wt) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }

        std::lock_guard<std::mutex> lk(fetch_state_mu_);
        auto& st = fetch_response_streams_[handler];
        st.connection_handle = conn;

        if (!st.write_handle) {
            const auto uni = wt->createUniStream();
            if (!uni) {
                SPDLOG_LOGGER_WARN(log_,
                                   "TryMoxygenFetchPublishObject: createUniStream failed ch={}",
                                   conn);
                return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
            }
            st.write_handle = uni.value();
            st.publish_fetch_header_sent = false;
        }

        if (!st.publish_fetch_header_sent) {
            quicr::messages::FetchHeader fetch_hdr;
            fetch_hdr.request_id = *req_id;
            quicr::Bytes hb;
            hb << fetch_hdr;
            const auto w = WriteMoqFetchResponseChunk(st.write_handle, { hb.data(), hb.size() }, false);
            if (w != quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
                FinAndClearMoqFetchStream(st);
                fetch_response_streams_.erase(handler);
                return w;
            }
            st.publish_fetch_header_sent = true;
        }

        const uint8_t priority =
          object_headers.priority.has_value() ? *object_headers.priority : handler->GetDefaultPriority();
        quicr::messages::FetchObject object{};
        object.group_id = object_headers.group_id;
        object.subgroup_id = object_headers.subgroup_id;
        object.object_id = object_headers.object_id;
        object.publisher_priority = priority;
        object.extensions = object_headers.extensions;
        object.immutable_extensions = object_headers.immutable_extensions;
        object.payload.assign(data.begin(), data.end());
        quicr::Bytes ob;
        ob << object;
        const auto w = WriteMoqFetchResponseChunk(st.write_handle, { ob.data(), ob.size() }, false);
        if (w != quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
            FinAndClearMoqFetchStream(st);
            fetch_response_streams_.erase(handler);
        }
        return w;
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
    MoxygenMoqServerPort::TryMoxygenFetchForwardPublishedData(quicr::PublishFetchHandler* handler,
                                                             bool is_new_stream,
                                                             uint64_t /*group_id*/,
                                                             uint64_t /*subgroup_id*/,
                                                             std::shared_ptr<const std::vector<uint8_t>> data)
    {
        if (!handler) {
            return std::nullopt;
        }
        const auto* laps_pf = dynamic_cast<const laps::PublishFetchHandler*>(handler);
        if (!laps_pf || !laps_pf->IsMoqFetchPortBound()) {
            return std::nullopt;
        }
        if (!data || data->empty()) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
        }
        const auto conn = handler->GetConnectionId();
        const auto session = LockMoqSession(conn);
        if (!session) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        if (!MoqSessionWebTransport(session)) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        const auto relay = std::dynamic_pointer_cast<laps::LapsMoqRelaySession>(session);
        if (!relay || !relay->getExecutor()) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        try {
            return folly::coro::blockingWait(folly::coro::co_withExecutor(
              relay->getExecutor(),
              folly::coro::co_invoke([this, handler, is_new_stream, data]() mutable
                                         -> folly::coro::Task<
                                           std::optional<quicr::PublishTrackHandler::PublishObjectStatus>> {
                  co_return TryMoxygenFetchForwardPublishedDataOnExecutor(handler, is_new_stream, std::move(data));
              })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log_, "TryMoxygenFetchForwardPublishedData: executor error ch={} ex={}", conn, ex.what());
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
    MoxygenMoqServerPort::TryMoxygenFetchForwardPublishedDataOnExecutor(
      quicr::PublishFetchHandler* handler,
      bool is_new_stream,
      std::shared_ptr<const std::vector<uint8_t>> data)
    {
        const auto conn = handler->GetConnectionId();
        const auto session = LockMoqSession(conn);
        if (!session) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        auto* const wt = MoqSessionWebTransport(session);
        if (!wt) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }

        std::lock_guard<std::mutex> lk(fetch_state_mu_);
        auto& st = fetch_response_streams_[handler];
        st.connection_handle = conn;

        if (is_new_stream) {
            FinAndClearMoqFetchStream(st);
        }

        if (!st.write_handle) {
            const auto uni = wt->createUniStream();
            if (!uni) {
                SPDLOG_LOGGER_WARN(log_,
                                   "TryMoxygenFetchForwardPublishedData: createUniStream failed ch={}",
                                   conn);
                return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
            }
            st.write_handle = uni.value();
        }

        const auto w =
          WriteMoqFetchResponseChunk(st.write_handle, { data->data(), data->size() }, false);
        if (w != quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
            FinAndClearMoqFetchStream(st);
            fetch_response_streams_.erase(handler);
        }
        return w;
    }

    void MoxygenMoqServerPort::FinAndClearMoqFetchStream(MoqFetchResponseStreamState& st)
    {
        if (st.write_handle) {
            (void)st.write_handle->writeStreamData(folly::IOBuf::create(0), true, nullptr);
        }
        st.write_handle = nullptr;
        st.publish_fetch_header_sent = false;
    }

    void MoxygenMoqServerPort::RemoveFetchResponseStreamLocked(quicr::PublishFetchHandler* handler)
    {
        const auto it = fetch_response_streams_.find(handler);
        if (it == fetch_response_streams_.end()) {
            return;
        }
        FinAndClearMoqFetchStream(it->second);
        fetch_response_streams_.erase(it);
    }

    void MoxygenMoqServerPort::ClearFetchResponseStreamsForConnectionLocked(
      quicr::ConnectionHandle connection_handle)
    {
        for (auto it = fetch_response_streams_.begin(); it != fetch_response_streams_.end();) {
            if (it->second.connection_handle == connection_handle) {
                FinAndClearMoqFetchStream(it->second);
                it = fetch_response_streams_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void MoxygenMoqServerPort::StoreUpstreamSubscription(
      UpstreamSubKey key, std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle)
    {
        std::lock_guard<std::mutex> lock(subs_mu_);
        upstream_subs_[std::move(key)] = std::move(handle);
    }

    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> MoxygenMoqServerPort::TakeUpstreamSubscription(
      const UpstreamSubKey& key)
    {
        std::lock_guard<std::mutex> lock(subs_mu_);
        const auto it = upstream_subs_.find(key);
        if (it == upstream_subs_.end()) {
            return nullptr;
        }
        auto h = std::move(it->second);
        upstream_subs_.erase(it);
        return h;
    }

    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> MoxygenMoqServerPort::FindUpstreamSubscription(
      const UpstreamSubKey& key) const
    {
        std::lock_guard<std::mutex> lock(subs_mu_);
        const auto it = upstream_subs_.find(key);
        return it == upstream_subs_.end() ? nullptr : it->second;
    }

    void MoxygenMoqServerPort::ClearUpstreamSubscriptionsFor(quicr::ConnectionHandle connection_handle)
    {
        std::lock_guard<std::mutex> lock(subs_mu_);
        for (auto it = upstream_subs_.begin(); it != upstream_subs_.end();) {
            if (it->first.first == connection_handle) {
                it = upstream_subs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void MoxygenMoqServerPort::StoreInboundSubscription(
      quicr::ConnectionHandle connection_handle,
      uint64_t subscribe_request_id,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle)
    {
        if (!handle) {
            return;
        }
        std::lock_guard<std::mutex> lock(inbound_mu_);
        inbound_subs_[InboundSubKey{ connection_handle, subscribe_request_id }] = std::move(handle);
    }

    void MoxygenMoqServerPort::ClearInboundSubscriptionsFor(quicr::ConnectionHandle connection_handle)
    {
        std::lock_guard<std::mutex> lock(inbound_mu_);
        for (auto it = inbound_subs_.begin(); it != inbound_subs_.end();) {
            if (it->first.first == connection_handle) {
                it = inbound_subs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void MoxygenMoqServerPort::RegisterRelayPublisherBinding(
      quicr::ConnectionHandle subscriber_connection_handle,
      uint64_t subscribe_request_id,
      const std::shared_ptr<quicr::PublishTrackHandler>& publish_handler)
    {
        if (!publish_handler) {
            return;
        }
        std::shared_ptr<moxygen::Publisher::SubscriptionHandle> sub_handle;
        {
            std::lock_guard<std::mutex> lock(inbound_mu_);
            const auto it = inbound_subs_.find(InboundSubKey{ subscriber_connection_handle, subscribe_request_id });
            if (it == inbound_subs_.end()) {
                SPDLOG_LOGGER_WARN(log_,
                                   "RegisterRelayPublisherBinding: no inbound subscription ch={} req_id={}",
                                   subscriber_connection_handle,
                                   subscribe_request_id);
                return;
            }
            sub_handle = it->second;
        }

        const auto session = LockMoqSession(subscriber_connection_handle);
        if (!session) {
            SPDLOG_LOGGER_WARN(log_,
                               "RegisterRelayPublisherBinding: no MoQSession ch={}",
                               subscriber_connection_handle);
            return;
        }

        auto binding = std::make_shared<RelayMoqBinding>();
        binding->subscriber_conn = subscriber_connection_handle;
        binding->session = session;
        binding->subscription_handle = std::move(sub_handle);

        std::lock_guard<std::mutex> lock(relay_mu_);
        relay_bindings_[publish_handler.get()] = std::move(binding);
    }

    void MoxygenMoqServerPort::UnregisterRelayPublisherBinding(quicr::PublishTrackHandler* publish_handler)
    {
        if (!publish_handler) {
            return;
        }
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay_bindings_.erase(publish_handler);
    }

    void MoxygenMoqServerPort::ClearRelayPublishBindingsForConnection(
      quicr::ConnectionHandle subscriber_connection_handle)
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        for (auto it = relay_bindings_.begin(); it != relay_bindings_.end();) {
            if (it->second && it->second->subscriber_conn == subscriber_connection_handle) {
                it = relay_bindings_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool MoxygenMoqServerPort::EnsureRelayTrackConsumer(const std::shared_ptr<RelayMoqBinding>& binding,
                                                      quicr::PublishTrackHandler& handler,
                                                      const std::shared_ptr<spdlog::logger>& log)
    {
        {
            std::lock_guard<std::mutex> lock(binding->consumer_mu);
            if (binding->consumer) {
                return true;
            }
        }

        const auto session = binding->session.lock();
        if (!session || !session->getExecutor() || !binding->subscription_handle) {
            SPDLOG_LOGGER_WARN(log, "EnsureRelayTrackConsumer: invalid session or subscription handle");
            return false;
        }

        struct ConsumerInit
        {
            std::shared_ptr<moxygen::TrackConsumer> consumer;
            bool ok{ false };
        };
        const auto out = std::make_shared<ConsumerInit>();

        try {
            folly::coro::blockingWait(folly::coro::co_withExecutor(
              session->getExecutor(),
              folly::coro::co_invoke(
              [session, sub_handle = binding->subscription_handle, &handler, out]() mutable -> folly::coro::Task<void> {
                  moxygen::PublishRequest pub = MoqShimBuildRelayPublishRequestForHandler(handler);
                  auto pr = session->publish(std::move(pub), sub_handle);
                  if (!pr.hasValue()) {
                      co_return;
                  }
                  auto consumer = pr.value().consumer;
                  auto reply = std::move(pr.value().reply);
                  const auto ok = co_await std::move(reply);
                  if (!ok.hasValue()) {
                      co_return;
                  }
                  out->consumer = std::move(consumer);
                  out->ok = true;
                  co_return;
              })));
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(log, "EnsureRelayTrackConsumer: blockingWait ex={}", ex.what());
            return false;
        }

        if (!out->ok || !out->consumer) {
            SPDLOG_LOGGER_WARN(log, "EnsureRelayTrackConsumer: publish handshake failed");
            return false;
        }

        std::lock_guard<std::mutex> lock(binding->consumer_mu);
        if (!binding->consumer) {
            binding->consumer = std::move(out->consumer);
        }
        return static_cast<bool>(binding->consumer);
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus> MoxygenMoqServerPort::TryRelayPublishObject(
      quicr::PublishTrackHandler* handler,
      quicr::TrackMode track_mode,
      const quicr::ObjectHeaders& object_headers,
      quicr::BytesSpan data)
    {
        if (!handler) {
            return std::nullopt;
        }

        std::shared_ptr<RelayMoqBinding> binding;
        {
            std::lock_guard<std::mutex> lock(relay_mu_);
            const auto it = relay_bindings_.find(handler);
            if (it == relay_bindings_.end()) {
                return std::nullopt;
            }
            binding = it->second;
        }

        if (!EnsureRelayTrackConsumer(binding, *handler, log_)) {
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }

        std::shared_ptr<moxygen::TrackConsumer> consumer;
        {
            std::lock_guard<std::mutex> lock(binding->consumer_mu);
            consumer = binding->consumer;
        }

        if (object_headers.status != quicr::ObjectStatus::kAvailable) {
            SPDLOG_LOGGER_DEBUG(log_,
                                "TryRelayPublishObject: non-NORMAL object status not forwarded on MoQ relay path");
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }

        auto payload = folly::IOBuf::copyBuffer(data.data(), static_cast<std::size_t>(data.size()));
        const auto moq_status = QuicrObjectStatusToMoq(object_headers.status);
        moxygen::ObjectHeader obj_hdr(object_headers.group_id,
                                      object_headers.subgroup_id,
                                      object_headers.object_id,
                                      object_headers.priority.value_or(handler->GetDefaultPriority()),
                                      moq_status,
                                      moxygen::noExtensions(),
                                      data.size());

        const quicr::TrackMode effective_mode =
          object_headers.track_mode.has_value() ? *object_headers.track_mode : track_mode;

        if (effective_mode == quicr::TrackMode::kDatagram) {
            const auto res = consumer->datagram(obj_hdr, std::move(payload), false);
            if (res.hasError()) {
                SPDLOG_LOGGER_WARN(log_, "TryRelayPublishObject: datagram failed");
                return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
            }
            return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
        }

        const auto res = consumer->objectStream(obj_hdr, std::move(payload), false);
        if (res.hasError()) {
            SPDLOG_LOGGER_WARN(log_, "TryRelayPublishObject: objectStream failed");
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        return quicr::PublishTrackHandler::PublishObjectStatus::kOk;
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus> MoxygenMoqServerPort::TryRelayForwardPublishedData(
      quicr::PublishTrackHandler* handler,
      bool /*is_new_stream*/,
      uint64_t /*group_id*/,
      uint64_t /*subgroup_id*/,
      std::shared_ptr<const std::vector<uint8_t>> /*data*/)
    {
        if (!handler) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(relay_mu_);
        if (relay_bindings_.find(handler) == relay_bindings_.end()) {
            return std::nullopt;
        }
        SPDLOG_LOGGER_DEBUG(
          log_,
          "TryRelayForwardPublishedData: pipelined stream chunks require raw stream wiring; returning error");
        return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
    }

} // namespace laps::moq::shim
