// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "peering/common.h"
#include "subscribe_info.h"

#include <quicr/detail/messages.h>
#include <quicr/detail/uintvar.h>

namespace laps::peering {

    enum class DataObjectType : uint8_t
    {
        kDatagram = 0,
        kExistingStream,
        kNewStream,
    };

    /// Minimum data object size in order to deserialize
    constexpr size_t kDataObjectMinSize =
      sizeof(DataObjectType) + sizeof(SubscribeNodeSetId) + sizeof(quicr::TrackFullNameHash) + 6;

    /**
     * @brief Data object to be sent to subscribers
     *
     * @details Data objects are initially enqueued at full size of the object, which may be very large.
     *    They are then sliced and transmitted based on QUIC-transport MTU and other byte limitations. Bytes
     *    are pipelined to relays to avoid hop by hop delays. The edge relay buffers up to the object length
     *     before transmitting to the client. This will change when the MoQT implementation supports pipelining.
     */
    class DataObject
    {
      public:
        // Below are sent only when include headers is requested
        uint8_t header_len{ 0 };        ///< Size of header length in bytes (up to start of payload bytes)
        DataObjectType type;            ///< Type of the data object
        SubscribeNodeSetId sns_id{ 0 }; ///< SNS ID used by the peer
        quicr::TrackFullNameHash track_full_name_hash{ 0 }; ///< Full Track name (aka track alias)

        uint8_t priority{ 1 }; ///< Stream only; Priority for new stream
        uint32_t ttl{ 2000 };  ///< Stream only; Time to live in millis for stream objects

        // Below are sent in every data object

        uint64_t data_length{ 0 }; ///< Length of data object (aka payload) as uintvar on wire

        /**
         * @brief Span of data (aka object payload)
         */
        Span<uint8_t const> data;

        /// Data storage is only used when deserializing doesn't have enough bytes for span
        std::vector<uint8_t> data_storage;

        /**
         * @brief Encode data object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize();

        bool Deserialize(Span<uint8_t const> serialized_data, bool parse_payload=true);

        DataObject() = default;
        DataObject(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataObjectType type);
        DataObject(Span<uint8_t const> serialized_data);

        std::pair<uint64_t, bool> AppendData(Span<uint8_t const> data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object);

} // namespace laps