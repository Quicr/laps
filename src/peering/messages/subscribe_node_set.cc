// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_node_set.h"

namespace laps::peering {

    uint32_t SubscribeNodeSet::SizeBytes(bool withdraw) const
    {
        if (withdraw) {
            return sizeof(SubscribeNodeSetId);
        }

        return kSnsAdvHeaderSize + nodes.size() * sizeof(NodeIdValueType);
    }

    SubscribeNodeSet::SubscribeNodeSet(std::span<const uint8_t> serialized_data, bool withdraw)
    {
        auto it = serialized_data.begin();

        id = ValueOf<uint32_t>({ it, it + 4 });
        it += 4;

        if (not withdraw) {
            priority = *it++;

            uint16_t num_nodes = ValueOf<uint16_t>({ it, it + 2 });
            it += 2;

            for (uint16_t i = 0; i < num_nodes; i++) {
                nodes.emplace(ValueOf<NodeIdValueType>({ it, it + sizeof(NodeIdValueType) }));
                it += sizeof(NodeIdValueType);
            }
        }
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeNodeSet& sns)
    {
        auto id_bytes = BytesOf(sns.id);
        data.insert(data.end(), id_bytes.rbegin(), id_bytes.rend());

        data.push_back(sns.priority);

        uint16_t num_nodes = sns.nodes.size();
        auto num_nodes_bytes = BytesOf(num_nodes);
        data.insert(data.end(), num_nodes_bytes.rbegin(), num_nodes_bytes.rend());

        for (auto& node_id : sns.nodes) {
            auto node_id_bytes = BytesOf(node_id);
            data.insert(data.end(), node_id_bytes.rbegin(), node_id_bytes.rend());
        }

        return data;
    }

    std::vector<uint8_t> SubscribeNodeSet::Serialize(bool include_common_header, bool withdraw) const
    {
        std::vector<uint8_t> data;

        if (include_common_header) {
            data.reserve(kCommonHeadersSize + SizeBytes(withdraw));
            data.push_back(kProtocolVersion);
            uint16_t type = static_cast<uint16_t>(withdraw ? MsgType::kSubscribeNodeSetWithdrawn
                                                           : MsgType::kSubscribeNodeSetAdvertised);
            auto type_bytes = BytesOf(type);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto sns_size = SizeBytes(withdraw);
            auto data_len_bytes = BytesOf(sns_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());
        } else {
            data.reserve(SizeBytes(withdraw));
        }

        if (withdraw) {
            auto id_bytes = BytesOf(id);
            data.insert(data.end(), id_bytes.rbegin(), id_bytes.rend());

        } else {
            data << *this;
        }

        return data;
    }
}
