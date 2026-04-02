// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "moxygen_quicr_subscribe_track_consumer.h"

#include <quicr/object.h>
#include <quicr/subscribe_track_handler.h>

#include <folly/io/IOBuf.h>

#include <vector>

namespace laps::moq::shim {

    namespace {

        std::vector<uint8_t> PayloadToBytes(const moxygen::Payload& payload)
        {
            if (!payload) {
                return {};
            }
            std::unique_ptr<folly::IOBuf> c = payload->clone();
            c->coalesce();
            return std::vector<uint8_t>(c->data(), c->data() + c->length());
        }

        quicr::ObjectStatus MapObjectStatus(moxygen::ObjectStatus s)
        {
            using M = moxygen::ObjectStatus;
            using Q = quicr::ObjectStatus;
            switch (s) {
            case M::NORMAL:
                return Q::kAvailable;
            case M::OBJECT_NOT_EXIST:
            case M::GROUP_NOT_EXIST:
                return Q::kDoesNotExist;
            case M::END_OF_GROUP:
                return Q::kEndOfGroup;
            case M::END_OF_TRACK:
                return Q::kEndOfTrack;
            default:
                return Q::kAvailable;
            }
        }

        quicr::ObjectHeaders ToQuicrHeaders(const moxygen::ObjectHeader& h, std::size_t payload_len)
        {
            quicr::ObjectHeaders o{};
            o.group_id = h.group;
            o.subgroup_id = h.subgroup;
            o.object_id = h.id;
            o.payload_length = payload_len;
            if (h.length.has_value()) {
                o.payload_length = *h.length;
            }
            o.status = MapObjectStatus(h.status);
            if (h.priority.has_value()) {
                o.priority = *h.priority;
            }
            return o;
        }

        class SubgroupToQuicr final : public moxygen::SubgroupConsumer
        {
          public:
            SubgroupToQuicr(std::shared_ptr<quicr::SubscribeTrackHandler> handler,
                            uint64_t group_id,
                            uint64_t subgroup_id)
              : handler_(std::move(handler))
              , group_id_(group_id)
              , subgroup_id_(subgroup_id)
            {
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
              uint64_t object_id,
              moxygen::Payload payload,
              moxygen::Extensions extensions,
              bool fin_subgroup) override
            {
                (void)extensions;
                (void)fin_subgroup;
                auto bytes = PayloadToBytes(payload);
                moxygen::ObjectHeader mh(group_id_, subgroup_id_, object_id);
                quicr::ObjectHeaders qh = ToQuicrHeaders(mh, bytes.size());
                handler_->ObjectReceived(qh, quicr::BytesSpan(bytes.data(), bytes.size()));
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
              uint64_t object_id,
              uint64_t length,
              moxygen::Payload initial_payload,
              moxygen::Extensions extensions) override
            {
                (void)extensions;
                streaming_object_id_ = object_id;
                expected_len_ = length;
                received_len_ = 0;
                buffer_.clear();
                if (initial_payload) {
                    auto chunk = PayloadToBytes(initial_payload);
                    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
                    received_len_ = buffer_.size();
                }
                if (received_len_ > expected_len_) {
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::Code::API_ERROR, "object length overflow"));
                }
                return folly::unit;
            }

            folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError> objectPayload(
              moxygen::Payload payload,
              bool fin_subgroup) override
            {
                (void)fin_subgroup;
                if (!payload) {
                    if (received_len_ == expected_len_) {
                        FlushObject();
                        return moxygen::ObjectPublishStatus::DONE;
                    }
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::Code::API_ERROR, "short object payload"));
                }
                auto chunk = PayloadToBytes(payload);
                buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
                received_len_ = buffer_.size();
                if (received_len_ > expected_len_) {
                    return folly::makeUnexpected(moxygen::MoQPublishError(
                      moxygen::MoQPublishError::Code::API_ERROR, "object length overflow"));
                }
                if (received_len_ == expected_len_) {
                    FlushObject();
                    return moxygen::ObjectPublishStatus::DONE;
                }
                return moxygen::ObjectPublishStatus::IN_PROGRESS;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(
              uint64_t end_of_group_object_id) override
            {
                (void)end_of_group_object_id;
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfTrackAndGroup(
              uint64_t end_of_track_object_id) override
            {
                (void)end_of_track_object_id;
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfSubgroup() override
            {
                return folly::unit;
            }

            void reset(moxygen::ResetStreamErrorCode error) override { (void)error; }

          private:
            void FlushObject()
            {
                moxygen::ObjectHeader mh(group_id_, subgroup_id_, streaming_object_id_);
                quicr::ObjectHeaders qh = ToQuicrHeaders(mh, buffer_.size());
                handler_->ObjectReceived(qh, quicr::BytesSpan(buffer_.data(), buffer_.size()));
                buffer_.clear();
                received_len_ = 0;
                expected_len_ = 0;
            }

            std::shared_ptr<quicr::SubscribeTrackHandler> handler_;
            uint64_t group_id_;
            uint64_t subgroup_id_;
            uint64_t streaming_object_id_{ 0 };
            uint64_t expected_len_{ 0 };
            uint64_t received_len_{ 0 };
            std::vector<uint8_t> buffer_;
        };

        class QuicrSubscribeTrackMoqConsumer final : public moxygen::TrackConsumer
        {
          public:
            explicit QuicrSubscribeTrackMoqConsumer(std::shared_ptr<quicr::SubscribeTrackHandler> handler)
              : handler_(std::move(handler))
            {
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> setTrackAlias(
              moxygen::TrackAlias alias) override
            {
                if (handler_) {
                    handler_->SetReceivedTrackAlias(alias.value);
                }
                return folly::unit;
            }

            folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError> beginSubgroup(
              uint64_t group_id,
              uint64_t subgroup_id,
              moxygen::Priority priority,
              bool contains_last_in_group) override
            {
                (void)priority;
                (void)contains_last_in_group;
                return std::static_pointer_cast<moxygen::SubgroupConsumer>(
                  std::make_shared<SubgroupToQuicr>(handler_, group_id, subgroup_id));
            }

            folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError> awaitStreamCredit()
              override
            {
                return folly::makeSemiFuture(folly::unit);
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> objectStream(
              const moxygen::ObjectHeader& header,
              moxygen::Payload payload,
              bool last_in_group) override
            {
                (void)last_in_group;
                auto bytes = PayloadToBytes(payload);
                quicr::ObjectHeaders qh = ToQuicrHeaders(header, bytes.size());
                if (handler_) {
                    handler_->ObjectReceived(qh, quicr::BytesSpan(bytes.data(), bytes.size()));
                }
                return folly::unit;
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> datagram(
              const moxygen::ObjectHeader& header,
              moxygen::Payload payload,
              bool last_in_group) override
            {
                return objectStream(header, std::move(payload), last_in_group);
            }

            folly::Expected<folly::Unit, moxygen::MoQPublishError> publishDone(
              moxygen::PublishDone pub_done) override
            {
                (void)pub_done;
                return folly::unit;
            }

          private:
            std::shared_ptr<quicr::SubscribeTrackHandler> handler_;
        };

    } // namespace

    std::shared_ptr<moxygen::TrackConsumer> MakeQuicrSubscribeTrackMoqConsumer(
      std::shared_ptr<quicr::SubscribeTrackHandler> handler)
    {
        return std::make_shared<QuicrSubscribeTrackMoqConsumer>(std::move(handler));
    }

} // namespace laps::moq::shim
