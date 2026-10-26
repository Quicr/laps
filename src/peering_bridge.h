// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <moq/peering/peer_manager.h>
#include <quicr/attributes.h>
#include <quicr/track_name.h>

#include <chrono>

namespace laps {

    inline moq::peering::TrackName ToPeeringTrack(const quicr::FullTrackName& full)
    {
        moq::peering::TrackName name;
        name.name = full.name;
        for (const auto& entry : full.name_space.GetEntries()) {
            name.name_space.emplace_back(entry.begin(), entry.end());
        }
        return name;
    }

    inline moq::peering::SubscribeParams ToPeeringParams(const quicr::SubscribeAttributes& attrs)
    {
        moq::peering::SubscribeParams params;
        params.priority = attrs.priority;
        params.delivery_timeout_ms = static_cast<uint64_t>(attrs.delivery_timeout.count());
        params.expires_ms = static_cast<uint64_t>(attrs.expires.count());
        params.forward = attrs.forward;
        params.new_group_request_id = attrs.new_group_request_id;
        return params;
    }

    inline quicr::TrackNamespace ToQuicrNamespace(const std::vector<std::vector<uint8_t>>& entries)
    {
        if (entries.empty()) {
            return {};
        }
        return quicr::TrackNamespace(entries);
    }

    inline quicr::FullTrackName ToQuicrTrack(const moq::peering::TrackName& name)
    {
        return { ToQuicrNamespace(name.name_space), name.name };
    }

    inline quicr::SubscribeAttributes ToQuicrParams(const moq::peering::SubscribeParams& params)
    {
        quicr::SubscribeAttributes attrs;
        attrs.priority = params.priority;
        attrs.delivery_timeout = std::chrono::milliseconds(static_cast<std::int64_t>(params.delivery_timeout_ms));
        attrs.expires = std::chrono::milliseconds(static_cast<std::int64_t>(params.expires_ms));
        attrs.forward = params.forward;
        attrs.new_group_request_id = params.new_group_request_id;
        return attrs;
    }

} // namespace laps
