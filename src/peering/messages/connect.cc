// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "connect.h"

namespace laps::peering {

    Connect::Connect(PeerMode mode, const NodeInfo& node_info)
      : mode(mode)
      , node_info(node_info)
    {
    }

    uint32_t Connect::SizeBytes() const
    {
        return sizeof(mode) + node_info.SizeBytes();
    }

    Connect::Connect(Span<uint8_t const> serialized_data)
    {
        auto it = serialized_data.begin();

        mode = static_cast<PeerMode>(*it++);

        Span<const uint8_t> data(it, serialized_data.end());
        node_info = NodeInfo(data);
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const Connect& connect)
    {
        // Get the size of in bytes to be written
        auto size = connect.SizeBytes();

        // Common header
        data.push_back(kProtocolVersion);
        uint16_t type = static_cast<uint16_t>(MsgType::kConnect);
        auto type_bytes = BytesOf(type);
        data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
        auto data_len_bytes = BytesOf(size);
        data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());

        data.push_back(static_cast<uint8_t>(connect.mode));
        auto ni_data = connect.node_info.Serialize();
        data.insert(data.end(), ni_data.begin(), ni_data.end());

        return data;
    }

    std::vector<uint8_t> Connect::Serialize() const
    {
        std::vector<uint8_t> data;
        data.reserve(kCommonHeadersSize + SizeBytes());
        data << *this;
        return data;
    }
}
