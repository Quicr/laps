// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "peering/common.h"
#include "subscribe_info.h"

#include <quicr/detail/messages.h>
#include <quicr/detail/uintvar.h>

namespace laps::peering {

    /**
     * @brief Data bytes to be sent to the O
     *
     * @details Subscriber info describes a subscrier of a specific track.
     *    This info is exchanged with the relay control server(s).
     */
    class DataObject
    {
      public:
        SubscribeNodeSetId sns_id;                     ///< SNS ID used by the peer
        quicr::TrackFullNameHash track_full_name_hash; ///< Full Track name (aka track alias)
        uint64_t data_length;                          ///< Length of data object (aka payload) as uintvar


        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header, bool withdraw = false) const;

        DataObject() = default;
        DataObject(NodeIdValueType source_node_id, const FullNameHash& full_name);
        DataObject(Span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object);

} // namespace laps