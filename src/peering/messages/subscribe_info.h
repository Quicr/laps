// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "node_info.h"
#include <memory>
#include <optional>
#include <set>

#include <quicr/track_name.h>

namespace laps {
    class SubscribeTrackHandler;
}

namespace laps::peering {

    /**
     * @brief SubscriberInfo describes a publisher
     *
     * @details Subscriber info describes a subscrier of a specific track.
     *    This info is exchanged with the relay control server(s).
     */
    class SubscribeInfo
    {
      public:
        ///< Incremental sequence number for subscribe info. Less value from current can be ignored, unless zero/wrap
        uint16_t seq{ 0 };

        NodeIdValueType source_node_id; ///< Id of the originating source node

        quicr::TrackHash track_hash; ///< Full name hash

        /// Track identity previously carried inside the MoQ subscribe message.
        quicr::TrackNamespace name_space;
        std::vector<uint8_t> name;

        /**
         * Subscribe attributes. Encoded on the wire as fixed-width integers, the same way as track_hash.
         * new_group_request_id is preceded by a presence byte and omitted when unset.
         */
        uint8_t priority{ 0 };
        uint64_t delivery_timeout{ 0 }; ///< Milliseconds
        uint64_t expires{ 0 };          ///< Milliseconds
        uint64_t forward{ 0 };          ///< Non-zero forwards data, zero pauses it
        std::optional<uint64_t> new_group_request_id;

        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header, bool withdraw = false, bool is_origin = false);

        SubscribeInfo()
          : track_hash({})
        {
        }

        SubscribeInfo(std::uint64_t, NodeIdValueType source_node_id, const quicr::TrackHash& track_hash);
        SubscribeInfo(std::span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeInfo& node_info);

} // namespace laps