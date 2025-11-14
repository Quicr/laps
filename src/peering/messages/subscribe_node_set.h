// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "peering/common.h"
#include "peering/messages/node_info.h"

#include <quicr/track_name.h>
#include <set>

namespace laps::peering {

    constexpr uint32_t kMaxSnsId = 0xFFFFFFFE;
    constexpr uint16_t kSnsAdvHeaderSize =
      sizeof(SubscribeNodeSetId) + sizeof(uint8_t /* priority */) + sizeof(uint16_t /* num nodes */);

    /**
     * @brief SubscriberInfo describes a publisher
     *
     * @details Subscriber info describes a subscrier of a specific track.
     *    This info is exchanged with the relay control server(s).
     */
    class SubscribeNodeSet
    {
      public:
        SubscribeNodeSetId id{ 0 };      ///< SNS ID that references this object
        uint8_t priority{ 2 };           ///< Priority to use for data context
        std::set<NodeIdValueType> nodes; ///< Set of source nodes for each subscriber

        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header, bool withdraw = false) const;

        SubscribeNodeSet() = default;
        SubscribeNodeSet(std::span<uint8_t const> serialized_data, bool withdraw = false);

        uint32_t SizeBytes(bool withdraw) const;

        bool operator<(const SubscribeNodeSet& other) const { return id < other.id; }
        bool operator==(const SubscribeNodeSet& other) const { return id == other.id; }
        bool operator>(const SubscribeNodeSet& other) const { return id > other.id; }

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeNodeSet& sns);

} // namespace laps