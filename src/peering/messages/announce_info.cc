// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "announce_info.h"

namespace laps::peering {

    AnnounceInfo::AnnounceInfo(NodeIdValueType source_node_id, const quicr::FullTrackName& full_name)
      : source_node_id(source_node_id)
      , name_space(full_name.name_space)
      , name(full_name.name)
    {
    }

    uint32_t AnnounceInfo::SizeBytes() const
    {
        // clang-format off
        return   sizeof(source_node_id)
               + sizeof(fullname_hash)
               + 1 /* num of namespace tuples */
               + sizeof(uint16_t) * name_space.GetEntries().size() + name_space.size()
               + sizeof(uint16_t) + name.size();

        // clang-format on
    }

    AnnounceInfo::AnnounceInfo(std::span<const uint8_t> serialized_data)
    {
        auto it = serialized_data.begin();

        source_node_id = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        fullname_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        uint8_t num_entries = *it;
        ++it;

        std::vector<std::span<const uint8_t>> entries;
        entries.reserve(num_entries);

        for (int i = 0; i < num_entries; i++) {
            const uint16_t len = ValueOf<uint16_t>({ it, it + 2 });
            it += 2;

            entries.emplace_back(it, it + len);
            it += len;
        }

        name_space = quicr::messages::TrackNamespace(entries);

        uint16_t name_size = ValueOf<uint16_t>({ it, it + 2 });
        it += 2;

        if (name_size) {
            name.assign(it, it + name_size);
            it += name_size;
        }
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const AnnounceInfo& announce_info)
    {
        uint16_t short_var;
        auto src_node_bytes = BytesOf(announce_info.source_node_id);
        data.insert(data.end(), src_node_bytes.rbegin(), src_node_bytes.rend());

        auto fullname_hash_bytes = BytesOf(announce_info.fullname_hash);
        data.insert(data.end(), fullname_hash_bytes.rbegin(), fullname_hash_bytes.rend());

        const auto& entries = announce_info.name_space.GetEntries();

        data.push_back(static_cast<uint8_t>(entries.size()));

        for (const auto& entry : entries) {
            short_var = entry.size();
            auto entry_size = BytesOf(short_var);
            data.insert(data.end(), entry_size.rbegin(), entry_size.rend());
            data.insert(data.end(), entry.begin(), entry.end());
        }

        short_var = announce_info.name.size();
        auto name_size = BytesOf(short_var);
        data.insert(data.end(), name_size.rbegin(), name_size.rend());
        if (announce_info.name.size()) {
            data.insert(data.end(), announce_info.name.begin(), announce_info.name.end());
        }

        return data;
    }

    std::vector<uint8_t> AnnounceInfo::Serialize(bool include_common_header, bool withdraw) const
    {
        std::vector<uint8_t> data;

        if (include_common_header) {
            data.reserve(kCommonHeadersSize + SizeBytes());
            data.push_back(kProtocolVersion);
            uint16_t type =
              static_cast<uint16_t>(withdraw ? MsgType::kAnnounceInfoWithdrawn : MsgType::kAnnounceInfoAdvertised);
            auto type_bytes = BytesOf(type);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto ni_size = SizeBytes();
            auto data_len_bytes = BytesOf(ni_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());
        } else {
            data.reserve(SizeBytes());
        }

        data << *this;
        return data;
    }
}
