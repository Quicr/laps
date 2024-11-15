// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_info.h"

namespace laps::peering {

    SubscribeInfo::SubscribeInfo(quicr::TrackFullNameHash id,
                                 NodeIdValueType source_node_id,
                                 const FullNameHash& full_name)
      : source_node_id(source_node_id)
      , full_name(full_name)
    {
    }

    uint32_t SubscribeInfo::SizeBytes() const
    {
        return sizeof(source_node_id) + 1 /* num of namespace tuples */
               + full_name.SizeBytes() + 4 /* size of sub data */ + subscribe_data.size();
    }

    SubscribeInfo::SubscribeInfo(Span<const uint8_t> serialized_data)
    {
        auto it = serialized_data.begin();

        source_node_id = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        uint8_t tuple_size = *it++;

        for (uint8_t i = 0; i < tuple_size; i++) {
            full_name.namespace_tuples.push_back(ValueOf<uint64_t>({ it, it + 8 }));
            it += 8;
        }

        full_name.name_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;
        full_name.ComputeFullNameHash();
        full_name.ComputeNamespaceHash();

        uint32_t sub_size = ValueOf<uint32_t>({ it, it + 4 });
        it += 4;

        subscribe_data.assign(it, it + sub_size);
        // it += sub_size;
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeInfo& subscribe_info)
    {
        auto src_node_bytes = BytesOf(subscribe_info.source_node_id);
        data.insert(data.end(), src_node_bytes.rbegin(), src_node_bytes.rend());

        data.push_back(static_cast<uint8_t>(subscribe_info.full_name.namespace_tuples.size()));

        for (uint8_t i = 0; i < subscribe_info.full_name.namespace_tuples.size(); i++) {
            auto ns_item_bytes = BytesOf(subscribe_info.full_name.namespace_tuples[i]);
            data.insert(data.end(), ns_item_bytes.rbegin(), ns_item_bytes.rend());
        }

        auto name_bytes = BytesOf(subscribe_info.full_name.name_hash);
        data.insert(data.end(), name_bytes.rbegin(), name_bytes.rend());

        auto sub_size_bytes = BytesOf(uint32_t(subscribe_info.subscribe_data.size()));
        data.insert(data.end(), sub_size_bytes.rbegin(), sub_size_bytes.rend());

        data.insert(data.end(), subscribe_info.subscribe_data.begin(), subscribe_info.subscribe_data.end());

        return data;
    }

    std::vector<uint8_t> SubscribeInfo::Serialize(bool include_common_header, bool withdraw) const
    {
        std::vector<uint8_t> data;

        if (include_common_header) {
            data.reserve(kCommonHeadersSize + SizeBytes());
            data.push_back(kProtocolVersion);
            uint16_t type =
              static_cast<uint16_t>(withdraw ? MsgType::kSubscribeInfoWithdrawn : MsgType::kSubscribeInfoAdvertised);
            auto type_bytes = BytesOf(type);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto si_size = SizeBytes();
            auto data_len_bytes = BytesOf(si_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());
        } else {
            data.reserve(SizeBytes());
        }

        data << *this;
        return data;
    }
}
