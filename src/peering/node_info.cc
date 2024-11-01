// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "node_info.h"

namespace laps::peering {

    uint64_t NodeInfo::SumSrtt() const
    {
        uint64_t sum {0};
        for (const auto& item: path) {
            sum += item.srtt_us;
        }

        return sum;
    }

    uint32_t NodeInfo::SizeBytes() const
    {
        return sizeof(id) + sizeof(type) + quicr::UintVar(contact.size()).Size() + contact.size() + sizeof(longitude) + sizeof(latitude) +
               (path.size() * sizeof(NodePathItem));
    }

    NodeInfo::NodeInfo(Span<uint8_t const> serialized_data)
    {
        auto it = serialized_data.begin();

        id = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        type = static_cast<NodeType>(*it++);

        auto contact_uv_len = quicr::UintVar::Size(*it);
        auto contact_len = uint64_t(quicr::UintVar({ it, it + contact_uv_len }));
        it += contact_uv_len;
        contact.assign(it, it + contact_len);
        it += contact_len;

        longitude = ValueOf<double>({ it, it + 8 });
        it += 8;

        latitude = ValueOf<double>({ it, it + 8 });
        it += 8;

        NodePathItem item;
        for (; it < serialized_data.end(); it += sizeof(NodePathItem)) {
            std::memcpy(&item, &*it, sizeof(NodePathItem));
            path.push_back(item);
        }
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const NodeInfo& node_info)
    {
        auto id_bytes = BytesOf(node_info.id);
        data.insert(data.end(), id_bytes.rbegin(), id_bytes.rend());

        data.push_back(static_cast<uint8_t>(node_info.type));

        auto contact_len = quicr::UintVar(node_info.contact.size());
        data.insert(data.end(), contact_len.begin(), contact_len.end());
        data.insert(data.end(), node_info.contact.begin(), node_info.contact.end());

        auto long_bytes = BytesOf(node_info.longitude);
        data.insert(data.end(), long_bytes.rbegin(), long_bytes.rend());

        auto lat_bytes = BytesOf(node_info.latitude);
        data.insert(data.end(), lat_bytes.rbegin(), lat_bytes.rend());

        for (const auto& path_ni : node_info.path) {
            data.insert(data.end(),
                        reinterpret_cast<const uint8_t*>(&path_ni),
                        reinterpret_cast<const uint8_t*>(&path_ni) + sizeof(NodePathItem));
        }

        return data;
    }

    std::vector<uint8_t> NodeInfo::Serialize(bool include_common_header) const
    {
        std::vector<uint8_t> data;

        if (include_common_header) {
            data.reserve(kCommonHeadersSize + SizeBytes());
            data.push_back(kProtocolVersion);
            uint16_t type = static_cast<uint16_t>(MsgType::kNodeInfoAdvertise);
            auto type_bytes = BytesOf(type);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto ni_size = SizeBytes();
            auto data_len_bytes = BytesOf(ni_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());
        }
        else {
            data.reserve(SizeBytes());
        }

        data << *this;
        return data;
    }

}
