// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "peering/common.h"
#include <sstream>

namespace laps::peering {

    enum class NodeType : uint8_t
    {
        kVia = 0, ///< node is a relay that only is used as a via/proxy. It is not an edge that has clients.
        kEdge,    ///< node is a relay edge that has clients
        kStub,    ///< node is a relay that is a stub edge
    };

    /**
     * @brief NodeId class to convert string formatted node ID to uint64 ID value
     *
     * @details node ID string format is as follows:
     *
     *      **Scheme:** <high value>:<low value>
     *          The colon is **REQUIRED**
     *
     *      <high value> and <low value> can be represented as follows:
     *      - Single unsigned 32 bit number
     *      - Dotted notation of 16 bits, such as <uint16_t>.<uint16_t>
     *
     *      **Example Node ID strings**
     *      - 1.2:1234
     *      - 1:1
     *      - 100.2:9001.2001
     *      - 123456:789.100
     *
     *      The converted uint64_t value to string will be formatted using the dotted notation.
     */
    using NodeIdValueType = uint64_t;
    class NodeId
    {
      public:
        NodeId() = default;

        uint64_t Value(const std::string& id)
        {
            auto mid_pos = id.find(":");

            if (mid_pos == id.npos) {
                throw std::runtime_error("Invalid node id format, does not contain ':'");
            }

            auto hi = NumericVaue(id.substr(0, mid_pos));
            auto low = NumericVaue(id.substr(++mid_pos));

            id_ = hi;
            id_ <<= 32;
            id_ |= low;
            return id_;
        }

        std::string Value(uint64_t id)
        {
            std::ostringstream val;
            val << static_cast<uint16_t>(id >> 48) << "." << static_cast<uint16_t>(id << 16 >> 48) << ":"
                << static_cast<uint16_t>(id << 32 >> 48) << "." << static_cast<uint16_t>(id << 48 >> 48);

            return val.str();
        }

      private:
        uint32_t NumericVaue(const std::string& value)
        {
            uint32_t n_value{ 0 };

            auto end_pos = value.find('.');
            if (end_pos == value.npos) {
                return std::stoul(value);
            }

            size_t pos = 0;
            for (int i = 0; i < 2; i++) {

                n_value <<= 16;

                const auto dot_value = std::stoul(value.substr(pos, end_pos));
                if (dot_value > 65535) {
                    throw std::runtime_error(
                      "Invalid node ID dotted format, cannot have dot values greater than 65535");
                }

                n_value |= 0xFFFF & std::stoul(value.substr(pos, end_pos));

                if (end_pos == value.npos) {
                    break;
                }

                pos = ++end_pos;
                end_pos = value.find('.', pos);
            }

            return n_value;
        }

        NodeIdValueType id_;
    };

    /**
     * @brief Node path item.
     * @details Encoding expects this struct to be fixed size, do not use variable length fields
     *      In other words, this will be memcpy upon serialization and deserialization
     */
    struct NodePathItem
    {
        NodeIdValueType id; ///< Id of the node that received the node info
        uint64_t srtt_us;   ///< SRTT in microseconds of the peer session that received the node info
    } __attribute__((__packed__, aligned(1)));

    /**
     * @brief NodeInfo within the relay network
     *
     * @details NodeInfo class is used to advertise nodes and to track in memory
     *      other nodes that exist in the network. All nodes advertise themselves
     *      and the nodes they know about.
     */
    class NodeInfo
    {
      public:
        NodeIdValueType id;  ///< Globally unique Node ID
        NodeType type;       ///< Relay Node Type
        std::string contact; ///< Relay moq host[:port]

        // Attributes
        double longitude{ 0 }; ///< 8 byte longitude value detailing the location of the local relay
        double latitude{ 0 };  ///< 8 byte latitude value detailing the location of the local relay

        /// Path of nodes that this node info has been seen by. When sending this node info, a new entry
        /// is added into this list upon sending. The value of this in the NIB does not contain self.
        std::vector<NodePathItem> path;

        /**
         * @brief  Best via relays that thd e node woulperfer to use if not direct
         * @details Lists in ascending order of preference, the nodes
         *      that are closest and best choices to itself. Node updates will update
         *      this list based on best selection algorithm.
         */
        // std::array<NodeIdValueType, kViaRelayMax> best_via_relays;

        NodeInfo() = default;

        NodeInfo(Span<uint8_t const> serialized);

        uint32_t SizeBytes() const;

        /**
         * @brief Sum (total) sRTT values in path
         * @returns the total of sRTTs for each path hop
         */
        uint64_t SumSrtt() const;

        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header = false, bool withdraw = false) const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const NodeInfo& node_info);
} // namespace laps