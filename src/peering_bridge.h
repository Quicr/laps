// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <moq/fabric/peer_manager.h>
#include <quicr/attributes.h>
#include <quicr/track_name.h>

#include <chrono>

namespace laps {

    inline moq::fabric::TrackName ToPeeringTrack(const quicr::FullTrackName& full)
    {
        moq::fabric::TrackName name;
        name.name = full.name;
        for (const auto& entry : full.name_space.GetEntries()) {
            name.name_space.emplace_back(entry.begin(), entry.end());
        }
        return name;
    }

    inline moq::fabric::SubscribeParams ToPeeringParams(const quicr::SubscribeAttributes& attrs)
    {
        moq::fabric::SubscribeParams params;
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

    inline quicr::FullTrackName ToQuicrTrack(const moq::fabric::TrackName& name)
    {
        return { ToQuicrNamespace(name.name_space), name.name };
    }

    /**
     * @brief Full-name hash moq-fabric uses for SendData, HasSubscribers, and Unsubscribe
     *
     * @details moq-fabric hashes a namespace tuple by tuple: the first tuple is CityHash64, and each
     *      later tuple is CityHash64 of the previous hash followed by that tuple. libquicr hashes the
     *      concatenated tuples as one value, and LAPS keeps that value as the MoQ track alias. The two
     *      hashes are equal for a namespace of one tuple. A namespace with several tuples has to use
     *      this hash on fabric calls.
     */
    inline std::uint64_t FabricTrackHash(const quicr::FullTrackName& full)
    {
        return moq::fabric::PeerManager::HashTrack(ToPeeringTrack(full));
    }

    inline quicr::SubscribeAttributes ToQuicrParams(const moq::fabric::SubscribeParams& params)
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
