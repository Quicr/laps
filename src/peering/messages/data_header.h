// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "peering/common.h"
#include "subscribe_info.h"

#include <quicr/detail/messages.h>
#include <quicr/detail/uintvar.h>

namespace laps::peering {

    enum class DataType : uint8_t
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
    class DataHeader
    {
      public:
        // @note: Header variables vary by type
        uint8_t header_len{ 0 };        ///< Size of header length in bytes (up to start of payload bytes)
        DataType type;                  ///< Type of the data object
        SubscribeNodeSetId sns_id{ 0 }; ///< SNS ID used by the peer
        quicr::TrackFullNameHash track_full_name_hash{ 0 }; ///< Full Track name (aka track alias)

        uint8_t priority{ 1 }; ///< Stream only; Priority for new stream
        uint32_t ttl{ 2000 };  ///< Stream only; Time to live in millis for stream objects

        /**
         * @brief Encode data hader into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize();

        /**
         * Deserialize read data from the network
         *
         * @param serialized_data
         * @return True if successful, false if not enough data
         */
        bool Deserialize(std::span<uint8_t const> serialized_data);

        DataHeader() = default;
        DataHeader(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataType type);
        DataHeader(std::span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataHeader& data_header);

} // namespace laps