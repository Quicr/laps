// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "node_info.h"
#include "peering/common.h"
#include "quicr/detail/base_track_handler.h"
#include "quicr/hash.h"

#include <set>

#include <quicr/track_name.h>

namespace laps::peering {

    /**
     * @brief AnnounceInfo describes a publisher
     *
     * @details Announce info describes a publisher. Supports prefix matching.
     *    This info is exchanged with the relay control server(s).
     */
    class AnnounceInfo
    {
      public:
        /**
         * Id of the originating source node
         *
         * When this node id matches self node, it means this is originated by this node, otherwise
         * it was learned via peering.
         */
        NodeIdValueType source_node_id;

        quicr::messages::TrackNamespace name_space;
        quicr::messages::TrackName name;
        quicr::TrackFullNameHash fullname_hash{ 0 };

        /**
         * @brief Encode node object into bytes that can be written on the wire
         */
        std::vector<uint8_t> Serialize(bool include_common_header, bool withdraw = false) const;

        AnnounceInfo() = default;
        AnnounceInfo(NodeIdValueType source_node_id, const quicr::FullTrackName& full_name);
        AnnounceInfo(std::span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const AnnounceInfo& announce_info);

} // namespace laps