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
        DataObjectType type; ///< Not serialized; Type of the data object

        // Below are sent only when include headers is requested

        SubscribeNodeSetId sns_id;                     ///< SNS ID used by the peer
        quicr::TrackFullNameHash track_full_name_hash; ///< Full Track name (aka track alias)

        uint8_t priority; ///< Stream only; Priority for new stream
        uint16_t ttl;     ///< Stream only; Time to live in millis for stream objects

        // Below are sent in every data object

        uint64_t data_length; ///< Length of data object (aka payload) as uintvar on wire

        Span<uint8_t> data; ///< Data payload (aka object data)

        /**
         * @brief Encode data object into bytes that can be written on the wire
         *
         * @param include_header     True to prepend common and full data object header
         * @param type               Data object type is used when including header. Header is different based on type
         */
        std::vector<uint8_t> Serialize(bool include_header, DataObjectType type) const;

        DataObject() = default;
        DataObject(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataObjectType type);
        DataObject(Span<uint8_t const> serialized_data);

        uint32_t SizeBytes(bool include_header, DataObjectType type) const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object);

} // namespace laps