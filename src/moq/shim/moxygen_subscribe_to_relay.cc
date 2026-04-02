// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "moxygen_subscribe_to_relay.h"

#include <moxygen/MoQTypes.h>

#include <quicr/detail/ctrl_message_types.h>
#include <quicr/subscribe_track_handler.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace laps::moq::shim {

    namespace {

        quicr::messages::GroupOrder MoxygenToQuicrGroupOrder(moxygen::GroupOrder order)
        {
            switch (order) {
            case moxygen::GroupOrder::OldestFirst:
                return quicr::messages::GroupOrder::kAscending;
            case moxygen::GroupOrder::NewestFirst:
                return quicr::messages::GroupOrder::kDescending;
            case moxygen::GroupOrder::Default:
            default:
                return quicr::messages::GroupOrder::kOriginalPublisherOrder;
            }
        }

        std::chrono::milliseconds DeliveryTimeoutFromParams(const moxygen::TrackRequestParameters& params)
        {
            const uint64_t delivery_key = static_cast<uint64_t>(moxygen::TrackRequestParamKey::DELIVERY_TIMEOUT);
            for (const auto& p : params) {
                if (p.key == delivery_key) {
                    return std::chrono::milliseconds(static_cast<int64_t>(p.asUint64));
                }
            }
            return std::chrono::milliseconds{ 0 };
        }

        uint8_t PublisherPriorityFromParams(const moxygen::TrackRequestParameters& params)
        {
            const uint64_t prio_key = static_cast<uint64_t>(moxygen::TrackRequestParamKey::PUBLISHER_PRIORITY);
            for (const auto& p : params) {
                if (p.key == prio_key) {
                    return static_cast<uint8_t>(std::min<uint64_t>(p.asUint64, 255ULL));
                }
            }
            return moxygen::kDefaultPriority;
        }

        bool NewGroupRequestInParams(const moxygen::TrackRequestParameters& params)
        {
            const uint64_t ngr_key = static_cast<uint64_t>(moxygen::TrackRequestParamKey::NEW_GROUP_REQUEST);
            for (const auto& p : params) {
                if (p.key == ngr_key) {
                    return true;
                }
            }
            return false;
        }

        /// If MoQ SUBSCRIPTION_FILTER (0x21) carries an absolute location, align quicr start_location (draft-16 TLV).
        void MergeSubscriptionFilterIntoSubscribeAttributes(const moxygen::TrackRequestParameters& params,
                                                            quicr::messages::SubscribeAttributes& attrs)
        {
            const uint64_t sf_key = static_cast<uint64_t>(moxygen::TrackRequestParamKey::SUBSCRIPTION_FILTER);
            for (const auto& p : params) {
                if (p.key != sf_key) {
                    continue;
                }
                const auto& sf = p.asSubscriptionFilter;
                if (sf.filterType != moxygen::LocationType::AbsoluteStart &&
                    sf.filterType != moxygen::LocationType::AbsoluteRange) {
                    continue;
                }
                if (!sf.location.has_value()) {
                    continue;
                }
                attrs.start_location.group = sf.location->group;
                attrs.start_location.object = sf.location->object;
                return;
            }
        }

        quicr::messages::PublishAttributes BuildQuicrPublishAttributesFromMoqPublish(const moxygen::PublishRequest& pub)
        {
            quicr::messages::PublishAttributes attrs{};
            attrs.track_full_name = MoxygenSubscribeToQuicrFullTrackName(pub.fullTrackName);
            attrs.track_alias = pub.trackAlias.value;
            attrs.priority = PublisherPriorityFromParams(pub.params);
            attrs.group_order = MoxygenToQuicrGroupOrder(pub.groupOrder);
            attrs.delivery_timeout = DeliveryTimeoutFromParams(pub.params);
            attrs.expires = std::chrono::milliseconds{ 0 };
            attrs.filter = quicr::messages::Filter{ std::monostate{} };
            attrs.forward = pub.forward ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
            attrs.is_publisher_initiated = false;
            if (pub.largest.has_value()) {
                attrs.start_location.group = pub.largest->group;
                attrs.start_location.object = pub.largest->object;
            } else {
                attrs.start_location.group = 0;
                attrs.start_location.object = 0;
            }
            attrs.dynamic_groups = NewGroupRequestInParams(pub.params);
            return attrs;
        }

    } // namespace

    quicr::FullTrackName MoxygenSubscribeToQuicrFullTrackName(const moxygen::FullTrackName& ftn)
    {
        std::vector<std::string> ns_parts = ftn.trackNamespace.trackNamespace;
        if (ns_parts.empty()) {
            ns_parts.emplace_back("");
        }
        quicr::TrackNamespace name_space(ns_parts);
        std::vector<uint8_t> name_bytes(ftn.trackName.begin(), ftn.trackName.end());
        return quicr::FullTrackName{ std::move(name_space), std::move(name_bytes) };
    }

    quicr::messages::SubscribeAttributes MoxygenSubscribeToQuicrAttributes(const moxygen::SubscribeRequest& sub)
    {
        quicr::messages::SubscribeAttributes attrs{};
        attrs.priority = sub.priority;
        attrs.group_order = MoxygenToQuicrGroupOrder(sub.groupOrder);
        attrs.delivery_timeout = DeliveryTimeoutFromParams(sub.params);
        attrs.expires = std::chrono::milliseconds{ 0 };
        attrs.filter = quicr::messages::Filter{ std::monostate{} };
        attrs.forward = sub.forward ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
        attrs.is_publisher_initiated = false;

        if (sub.locType == moxygen::LocationType::NextGroupStart) {
            attrs.new_group_request_id = std::optional<std::uint64_t>{ 1 };
        }

        switch (sub.locType) {
        case moxygen::LocationType::AbsoluteStart:
        case moxygen::LocationType::AbsoluteRange:
            if (sub.start.has_value()) {
                attrs.start_location.group = sub.start->group;
                attrs.start_location.object = sub.start->object;
            }
            break;
        case moxygen::LocationType::LargestObject:
        case moxygen::LocationType::LargestGroup:
            attrs.start_location.group = 0;
            attrs.start_location.object = 0;
            break;
        default:
            attrs.start_location.group = 0;
            attrs.start_location.object = 0;
            break;
        }

        MergeSubscriptionFilterIntoSubscribeAttributes(sub.params, attrs);

        return attrs;
    }

    namespace {

        moxygen::GroupOrder QuicrToMoxygenGroupOrder(quicr::messages::GroupOrder order)
        {
            switch (order) {
            case quicr::messages::GroupOrder::kAscending:
                return moxygen::GroupOrder::OldestFirst;
            case quicr::messages::GroupOrder::kDescending:
                return moxygen::GroupOrder::NewestFirst;
            case quicr::messages::GroupOrder::kOriginalPublisherOrder:
            default:
                return moxygen::GroupOrder::Default;
            }
        }

    } // namespace

    moxygen::FullTrackName QuicrFullTrackNameToMoxygen(const quicr::FullTrackName& q)
    {
        moxygen::TrackNamespace tn;
        for (const auto& ent : q.name_space.GetEntries()) {
            tn.trackNamespace.emplace_back(reinterpret_cast<const char*>(ent.data()), ent.size());
        }
        if (tn.trackNamespace.empty()) {
            tn.trackNamespace.emplace_back("");
        }
        std::string name(q.name.begin(), q.name.end());
        return moxygen::FullTrackName{ std::move(tn), std::move(name) };
    }

    moxygen::TrackNamespace QuicrTrackNamespaceToMoxygen(const quicr::TrackNamespace& q)
    {
        moxygen::TrackNamespace tn;
        for (const auto& ent : q.GetEntries()) {
            tn.trackNamespace.emplace_back(reinterpret_cast<const char*>(ent.data()), ent.size());
        }
        if (tn.trackNamespace.empty()) {
            tn.trackNamespace.emplace_back("");
        }
        return tn;
    }

    moxygen::SubscribeRequest MoqShimBuildUpstreamSubscribeRequest(const quicr::SubscribeTrackHandler& handler)
    {
        const auto mftn = QuicrFullTrackNameToMoxygen(handler.GetFullTrackName());
        std::vector<moxygen::Parameter> params;

        if (const auto dt = handler.GetDeliveryTimeout(); dt.has_value() && dt->count() > 0) {
            params.emplace_back(static_cast<uint64_t>(moxygen::TrackRequestParamKey::DELIVERY_TIMEOUT),
                                static_cast<uint64_t>(dt->count()));
        }

        auto loc_type = moxygen::LocationType::LargestObject;
        std::optional<moxygen::AbsoluteLocation> start;
        if (const auto loc = handler.GetLatestLocation()) {
            loc_type = moxygen::LocationType::AbsoluteStart;
            start = moxygen::AbsoluteLocation(loc->group, loc->object);
        }

        return moxygen::SubscribeRequest::make(mftn,
                                               handler.GetPriority(),
                                               QuicrToMoxygenGroupOrder(handler.GetGroupOrder()),
                                               true,
                                               loc_type,
                                               start,
                                               0,
                                               params);
    }

    quicr::messages::PublishAttributes MoqShimMoxygenPublishRequestToQuicrPublishAttributes(
      const moxygen::PublishRequest& pub)
    {
        return BuildQuicrPublishAttributesFromMoqPublish(pub);
    }

} // namespace laps::moq::shim
