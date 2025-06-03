// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_info.h"

namespace laps::peering {

    SubscribeInfo::SubscribeInfo(quicr::TrackFullNameHash id,
                                 NodeIdValueType source_node_id,
                                 const quicr::TrackHash& track_hash)
      : source_node_id(source_node_id)
      , track_hash(track_hash)
    {
    }

    uint32_t SubscribeInfo::SizeBytes() const
    {
        return sizeof(seq) + sizeof(source_node_id) + 24 /* namespace, name, and full name hashes */
               + 4 /* size of sub data */ + subscribe_data.size();
    }

    SubscribeInfo::SubscribeInfo(std::span<const uint8_t> serialized_data)
      : track_hash({})
    {
        auto it = serialized_data.begin();

        seq = ValueOf<uint16_t>({ it, it + 2 });
        it += 2;

        source_node_id = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        track_hash.track_namespace_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;
        track_hash.track_name_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;
        track_hash.track_fullname_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        uint32_t sub_size = ValueOf<uint32_t>({ it, it + 4 });
        it += 4;

        if (sub_size == 0) {
            return;
        }

        if (sub_size > serialized_data.end() - it) {
            throw std::out_of_range("Subscribe data size is larger than serialized data size");
        }

        subscribe_data.assign(it, it + sub_size);
        // it += sub_size;
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeInfo& subscribe_info)
    {
        auto seq_bytes = BytesOf(subscribe_info.seq);
        data.insert(data.end(), seq_bytes.rbegin(), seq_bytes.rend());

        auto src_node_bytes = BytesOf(subscribe_info.source_node_id);
        data.insert(data.end(), src_node_bytes.rbegin(), src_node_bytes.rend());

        auto namespace_bytes = BytesOf(subscribe_info.track_hash.track_namespace_hash);
        data.insert(data.end(), namespace_bytes.rbegin(), namespace_bytes.rend());

        auto name_bytes = BytesOf(subscribe_info.track_hash.track_name_hash);
        data.insert(data.end(), name_bytes.rbegin(), name_bytes.rend());

        auto full_name_bytes = BytesOf(subscribe_info.track_hash.track_fullname_hash);
        data.insert(data.end(), full_name_bytes.rbegin(), full_name_bytes.rend());

        uint32_t sub_size = subscribe_info.subscribe_data.size();
        auto sub_size_bytes = BytesOf(sub_size);
        data.insert(data.end(), sub_size_bytes.rbegin(), sub_size_bytes.rend());

        data.insert(data.end(), subscribe_info.subscribe_data.begin(), subscribe_info.subscribe_data.end());

        return data;
    }

    std::vector<uint8_t> SubscribeInfo::Serialize(bool include_common_header, bool withdraw, bool is_origin)
    {
        std::vector<uint8_t> data;

        if (is_origin) {
            if (seq < 0xFFFF)
                seq++; // Bump the sequence number
            else
                seq = 0;
        }

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
