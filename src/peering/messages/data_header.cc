// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "data_header.h"
#include <iomanip>

#include "subscribe_node_set.h"

namespace laps::peering {

    DataHeader::DataHeader(SubscribeNodeSetId sns_id, quicr::TrackFullNameHash full_name, DataType type)
      : type(type)
      , sns_id(sns_id)
      , track_full_name_hash(full_name)
    {
    }

    uint32_t DataHeader::SizeBytes() const
    {
        if (header_len) {
            return header_len;
        }

        uint32_t size = sizeof(header_len) + sizeof(type);

        switch (type) {
            case DataType::kDatagram:
                size += sizeof(sns_id) + sizeof(track_full_name_hash);
                break;

            case DataType::kExistingStream:
                break;

            case DataType::kNewStream:
                size += sizeof(sns_id) + sizeof(track_full_name_hash);
                size += sizeof(priority) + sizeof(ttl);
                break;
        }

        return size;
    }

    DataHeader::DataHeader(std::span<const uint8_t> serialized_data)
    {
        Deserialize(serialized_data);
    }

    bool DataHeader::Deserialize(std::span<const uint8_t> serialized_data)
    {
        if (serialized_data.empty()) {
            return false;
        }

        auto it = serialized_data.begin();

        header_len = *it++;

        if (header_len > serialized_data.size())
            throw std::invalid_argument("Serialized data is too short");

        type = static_cast<DataType>(*it++);

        switch (type) {
            case DataType::kExistingStream:
                break;

            case DataType::kDatagram: {
                sns_id = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;

                track_full_name_hash = ValueOf<uint64_t>({ it, it + 8 });
                it += 8;

                break;
            }

            case DataType::kNewStream: {
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

        return false;
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataHeader& data_object)
    {
        if (data_object.type == DataType::kExistingStream) {
            // No header
            return data;
        }

        data.push_back(data_object.header_len); // Will be set after knowing the full header size
        uint8_t* header_len = &data[0];
        *header_len = 2; // header length and type

        data.push_back(static_cast<uint8_t>(data_object.type));

        switch (data_object.type) {
            case DataType::kExistingStream:
                break;

            case DataType::kDatagram: {
                auto sns_id_bytes = BytesOf(data_object.sns_id);
                data.insert(data.end(), sns_id_bytes.rbegin(), sns_id_bytes.rend());

                auto tfn_bytes = BytesOf(data_object.track_full_name_hash);
                data.insert(data.end(), tfn_bytes.rbegin(), tfn_bytes.rend());

                *header_len += sns_id_bytes.size() + tfn_bytes.size();
                break;
            }

            case DataType::kNewStream: {
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

        return data;
    }

    std::vector<uint8_t> DataHeader::Serialize()
    {
        std::vector<uint8_t> net_data;

        net_data.reserve(SizeBytes());

        net_data << *this;
        return net_data;
    }
}
