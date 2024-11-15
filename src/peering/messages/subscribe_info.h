// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "node_info.h"
#include "peering/common.h"
#include <set>

#include <quicr/track_name.h>

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
        NodeIdValueType source_node_id; ///< Id of the originating source node

        FullNameHash full_name; ///< Full name hash
        std::vector<uint8_t>
          subscribe_data; /// Original MoQ subscribe message (wire format) that initiated this subscribe

        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header, bool withdraw = false) const;

        SubscribeInfo() = default;
        SubscribeInfo(quicr::TrackFullNameHash, NodeIdValueType source_node_id, const FullNameHash& full_name);
        SubscribeInfo(Span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeInfo& node_info);

} // namespace laps