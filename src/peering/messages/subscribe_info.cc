// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "subscribe_info.h"

#include <stdexcept>

namespace laps::peering {
    namespace {
        template<typename T>
        T Read(std::span<const uint8_t>::iterator& it, const std::span<const uint8_t>::iterator end)
        {
            if (static_cast<std::size_t>(end - it) < sizeof(T)) {
                throw std::out_of_range("Subscribe info truncated");
            }

            const T value = ValueOf<T>({ it, it + static_cast<std::ptrdiff_t>(sizeof(T)) });
            it += sizeof(T);
            return value;
        }

        template<typename T>
        void Append(std::vector<uint8_t>& data, const T& value)
        {
            const auto bytes = BytesOf(value);
            data.insert(data.end(), bytes.rbegin(), bytes.rend());
        }
    }

    SubscribeInfo::SubscribeInfo([[maybe_unused]] std::uint64_t track_fullname_hash,
                                 NodeIdValueType source_node_id,
                                 const quicr::TrackHash& track_hash)
      : source_node_id(source_node_id)
      , track_hash(track_hash)
    {
    }

    uint32_t SubscribeInfo::SizeBytes() const
    {
        return sizeof(seq) + sizeof(source_node_id) + 24 /* namespace, name, and full name hashes */
               + sizeof(priority) + sizeof(delivery_timeout) + sizeof(expires) + sizeof(forward) +
               1                                                               /* new group present */
               + (new_group_request_id.has_value() ? sizeof(uint64_t) : 0) + 1 /* namespace entry count */
               + sizeof(uint16_t) * static_cast<uint32_t>(name_space.GetEntries().size()) +
               static_cast<uint32_t>(name_space.size()) + sizeof(uint16_t) + static_cast<uint32_t>(name.size());
    }

    SubscribeInfo::SubscribeInfo(std::span<const uint8_t> serialized_data)
      : track_hash({})
    {
        auto it = serialized_data.begin();
        const auto end = serialized_data.end();

        seq = Read<uint16_t>(it, end);
        source_node_id = Read<uint64_t>(it, end);

        track_hash.track_namespace_hash = Read<uint64_t>(it, end);
        track_hash.track_name_hash = Read<uint64_t>(it, end);
        track_hash.track_fullname_hash = Read<uint64_t>(it, end);

        priority = Read<uint8_t>(it, end);
        delivery_timeout = Read<uint64_t>(it, end);
        expires = Read<uint64_t>(it, end);
        forward = Read<uint64_t>(it, end);

        if (Read<uint8_t>(it, end) != 0) {
            new_group_request_id = Read<uint64_t>(it, end);
        }

        const auto num_entries = Read<uint8_t>(it, end);
        std::vector<std::span<const uint8_t>> entries;
        entries.reserve(num_entries);

        for (uint8_t i = 0; i < num_entries; ++i) {
            const auto len = Read<uint16_t>(it, end);
            if (static_cast<std::size_t>(end - it) < len) {
                throw std::out_of_range("Subscribe namespace entry exceeds serialized data");
            }

            entries.emplace_back(it, it + len);
            it += len;
        }

        name_space = quicr::TrackNamespace(std::span<const std::span<const uint8_t>>{ entries });

        const auto name_size = Read<uint16_t>(it, end);
        if (static_cast<std::size_t>(end - it) < name_size) {
            throw std::out_of_range("Subscribe name exceeds serialized data");
        }

        if (name_size != 0) {
            name.assign(it, it + name_size);
        }
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const SubscribeInfo& subscribe_info)
    {
        Append(data, subscribe_info.seq);
        Append(data, subscribe_info.source_node_id);

        Append(data, subscribe_info.track_hash.track_namespace_hash);
        Append(data, subscribe_info.track_hash.track_name_hash);
        Append(data, subscribe_info.track_hash.track_fullname_hash);

        Append(data, subscribe_info.priority);
        Append(data, subscribe_info.delivery_timeout);
        Append(data, subscribe_info.expires);
        Append(data, subscribe_info.forward);

        Append(data, static_cast<uint8_t>(subscribe_info.new_group_request_id.has_value() ? 1 : 0));
        if (subscribe_info.new_group_request_id.has_value()) {
            Append(data, *subscribe_info.new_group_request_id);
        }

        const auto& entries = subscribe_info.name_space.GetEntries();
        data.push_back(static_cast<uint8_t>(entries.size()));
        for (const auto& entry : entries) {
            Append(data, static_cast<uint16_t>(entry.size()));
            data.insert(data.end(), entry.begin(), entry.end());
        }

        Append(data, static_cast<uint16_t>(subscribe_info.name.size()));
        if (!subscribe_info.name.empty()) {
            data.insert(data.end(), subscribe_info.name.begin(), subscribe_info.name.end());
        }

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
