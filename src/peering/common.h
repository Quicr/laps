// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <quicr/detail/uintvar.h>

#include <iostream>
#include <quicr/track_name.h>

namespace laps::peering {
#if __cplusplus >= 202002L
    constexpr bool kIsBigEndian = std::endian::native == std::endian::big;
#else
    constexpr bool kIsBigEndian = static_cast<const std::uint8_t&>(0x0001) == 0x00;
#endif

    constexpr int kViaRelayMax = 5; ///< Maximum number of best via relays to advertise
    constexpr uint8_t kProtocolVersion = 1;

    using HashType = uint64_t; ///< Value data type for hashes
    using NamespaceTuples = std::vector<HashType>;
    using PeerSessionId = uint64_t;
    using SubscribeNodeSetId = uint32_t;

    /**
     * @brief Peering Mode that the peer operates in to exchange info and/or data
     */
    enum class PeerMode : uint8_t
    {
        kIbp = 0, ///< Information base peering (control plane)
        kData,    ///< Data object peering (data plane)
        kBoth     ///< Does both IBP and DATA via the peering session
    };

    /**
     * @brief Peering message type
     */
    enum class MsgType : uint16_t
    {
        kConnect = 1,
        kConnectResponse,

        kDataObject,

        kNodeInfoAdvertise,
        kNodeInfoWithdrawn,
        kSubscribeInfoAdvertised,
        kSubscribeInfoWithdrawn,
        kAnnounceInfoAdvertised,
        kAnnounceInfoWithdrawn,
        kSubscribeNodeSetAdvertised,
        kSubscribeNodeSetWithdrawn
    };

    struct FullNameHash
    {
        NamespaceTuples namespace_tuples;
        HashType namespace_hash{ 0 };
        HashType name_hash{ 0 };
        HashType full_name_hash{ 0 };

        uint64_t ComputeFullNameHash()
        {
            std::hash<HashType> hasher;
            full_name_hash = 0;
            for (auto ns : namespace_tuples) {
                full_name_hash ^= hasher(ns) + 0x9e3779b9 + (full_name_hash << 6) + (full_name_hash >> 2);
            }

            full_name_hash ^= hasher(name_hash) + 0x9e3779b9 + (full_name_hash << 6) + (full_name_hash >> 2);
            full_name_hash = (full_name_hash << 2) >> 2;

            return full_name_hash;
        }

        uint64_t ComputeNamespaceHash()
        {
            std::hash<HashType> hasher;
            namespace_hash = 0;
            for (auto ns : namespace_tuples) {
                namespace_hash ^= hasher(ns) + 0x9e3779b9 + (namespace_hash << 6) + (namespace_hash >> 2);
            }

            return namespace_hash;
        }

        size_t SizeBytes() const { return namespace_tuples.size() * 8 + 8; }
    };

    constexpr uint16_t kCommonHeadersSize = 7; ///< Size of the headers in bytes
    /**
     * @brief Common headers that are part of every message sent
     */
    class CommonHeaders
    {
      public:
        uint8_t version;      ///< Version of the protocol
        uint16_t type;        ///< Type of message being sent
        uint32_t data_length; ///< Length of message data, **NOT** including common headers

        CommonHeaders() { version = kProtocolVersion; }
    };

    template<typename T, std::enable_if_t<std::is_integral<T>::value || std::is_floating_point<T>::value, bool> = true>
    std::span<const uint8_t> BytesOf(const T& value)
    {
        return std::span(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    }

    template<typename T, std::enable_if_t<std::is_integral<T>::value || std::is_floating_point<T>::value, bool> = true>
    constexpr T ValueOf(std::span<uint8_t const> value, bool host_order = true)
    {
        T rval{ 0 };
        auto rval_ptr = reinterpret_cast<uint8_t*>(&rval);

        if (kIsBigEndian || !host_order) {
            for (size_t i = 0; i < sizeof(T); i++) {
                rval_ptr[i] = value[i];
            }
        } else {
            constexpr auto last = sizeof(T) - 1;
            for (size_t i = 0; i < sizeof(T); i++) {
                rval_ptr[i] = value[last - i];
            }
        }

        return rval;
    }

} // namespace laps
