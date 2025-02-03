// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "data_object.h"
#include <iomanip>

#include "subscribe_node_set.h"

namespace laps::peering {

    DataObject::DataObject(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataObjectType type)
      : type(type)
      , sns_id(sns_id)
      , track_full_name_hash(full_name)
    {
    }

    uint32_t DataObject::SizeBytes() const
    {
        if (header_len) {
            return header_len + data.size();
        }

        uint32_t size = sizeof(header_len) + sizeof(type)
                        + quicr::UintVar(data.size()).Size() + data.size();

        switch (type) {
            case DataObjectType::kDatagram:
                size += sizeof(sns_id) + sizeof(track_full_name_hash);
                break;

            case DataObjectType::kExistingStream:
                break;

            case DataObjectType::kNewStream:
                size += sizeof(sns_id) + sizeof(track_full_name_hash);
                size += sizeof(priority) + sizeof(ttl);
                break;
        }

        return size;
    }

    DataObject::DataObject(Span<const uint8_t> serialized_data)
    {
        Deserialize(serialized_data);
    }

    bool DataObject::Deserialize(Span<const uint8_t> serialized_data, bool parse_payload)
    {
        if (serialized_data.empty()) false;

        auto it = serialized_data.begin();

        header_len = *it++;

        if (header_len > serialized_data.size())
            throw std::invalid_argument("Serialized data is too short");

        type = static_cast<DataObjectType>(*it++);

        switch (type) {
            case DataObjectType::kExistingStream:
                break;

            case DataObjectType::kDatagram: {
                sns_id = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;

                track_full_name_hash = ValueOf<uint64_t>({ it, it + 8 });
                it += 8;

                break;
            }

            case DataObjectType::kNewStream: {
                sns_id = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;

                track_full_name_hash = ValueOf<uint64_t>({ it, it + 8 });
                it += 8;

                priority = *it++;
                ttl = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;
                break;
            }
        }

        auto data_len_uv_size = quicr::UintVar::Size(*it);
        data_length = uint64_t(quicr::UintVar({ it, it + data_len_uv_size }));
        it += data_len_uv_size;

        if (parse_payload) {
            auto remaining_data_size = serialized_data.end() - it;
            if (data_length <= remaining_data_size) {
                data = {it, it + data_length};
                return true;
            }

            if (data_length >= 2'000'000) {
                SPDLOG_WARN("Received data object is very large size: {}", data_length);
                return false;
            }

            data_storage.reserve(data_length);
            data_storage.insert(data_storage.end(), it, serialized_data.end());

            data = {data_storage.begin(), data_storage.end()};

        } else {
            return true;
        }

        return false;
    }

    std::pair<uint64_t, bool> DataObject::AppendData(Span<const uint8_t> a_data)
    {
        uint64_t read_bytes{ 0 };

        const auto remaining_data_bytes = data_length - data.size();

        if (a_data.size() >= remaining_data_bytes) {
            data_storage.insert(data_storage.end(), a_data.begin(), a_data.begin() + remaining_data_bytes);
            read_bytes = remaining_data_bytes;
        } else {
            data_storage.insert(data_storage.end(), a_data.begin(), a_data.end());
            read_bytes = a_data.size();
        }

        data = { data_storage.begin(), data_storage.end() };

        return {read_bytes, data.size() == data_length};
    }


    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object)
    {
        data.push_back(data_object.header_len); // Will be set after knowing the full header size
        uint8_t* header_len = &data[0];
        *header_len = 2; // header length and type

        data.push_back(static_cast<uint8_t>(data_object.type));

        switch (data_object.type) {
            case DataObjectType::kExistingStream:
                break;

            case DataObjectType::kDatagram: {
                auto sns_id_bytes = BytesOf(data_object.sns_id);
                data.insert(data.end(), sns_id_bytes.rbegin(), sns_id_bytes.rend());

                auto tfn_bytes = BytesOf(data_object.track_full_name_hash);
                data.insert(data.end(), tfn_bytes.rbegin(), tfn_bytes.rend());

                *header_len += sns_id_bytes.size() + tfn_bytes.size();
                break;
            }

            case DataObjectType::kNewStream: {
                auto sns_id_bytes = BytesOf(data_object.sns_id);
                data.insert(data.end(), sns_id_bytes.rbegin(), sns_id_bytes.rend());

                auto tfn_bytes = BytesOf(data_object.track_full_name_hash);
                data.insert(data.end(), tfn_bytes.rbegin(), tfn_bytes.rend());

                data.push_back(data_object.priority);

                auto ttl_bytes = BytesOf(data_object.ttl);
                data.insert(data.end(), ttl_bytes.rbegin(), ttl_bytes.rend());

                *header_len += sns_id_bytes.size() + tfn_bytes.size() + sizeof(data_object.priority) + ttl_bytes.size();

                break;
            }
        }

        auto data_len_uv_size = quicr::UintVar(data_object.data_length);
        data.insert(data.end(), data_len_uv_size.begin(), data_len_uv_size.end());
        *header_len += data_len_uv_size.size();

        data.insert(data.end(), data_object.data.begin(), data_object.data.end());

        return data;
    }

    std::vector<uint8_t> DataObject::Serialize()
    {
        std::vector<uint8_t> net_data;

        data_length = data.size();

        net_data.reserve(SizeBytes());

        net_data << *this;
        return net_data;
    }
}
