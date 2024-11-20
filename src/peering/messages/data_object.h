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
        DataObjectType type;                           ///< Type of the data object
        SubscribeNodeSetId sns_id;                     ///< SNS ID used by the peer
        quicr::TrackFullNameHash track_full_name_hash; ///< Full Track name (aka track alias)

        uint8_t priority{ 1 }; ///< Stream only; Priority for new stream
        uint16_t ttl{ 2000 };  ///< Stream only; Time to live in millis for stream objects

        // Below are sent in every data object

        uint64_t data_length; ///< Length of data object (aka payload) as uintvar on wire

        /**
         * @brief Span of data (aka object payload)
         *
         * @note This is a bit dangerous because:
         *    1. app must not allow this data to go out of scope
         *      till the object is finished. PeerManager::ClientDataObject() and other peering
         *      methods are designed to deal with this.
         *    2. Deserialized data must not go out of scope either. Peering receive data handles this.
         */
        Span<uint8_t const> data;

        /**
         * @brief Encode data object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize() const;

        DataObject() = default;
        DataObject(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataObjectType type);
        DataObject(Span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object);

} // namespace laps