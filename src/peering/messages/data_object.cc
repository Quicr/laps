// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "data_object.h"

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
        uint32_t size = quicr::UintVar(data.size()).Size() + data.size();

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
        auto it = serialized_data.begin();

        type = static_cast<DataObjectType>(*(it++));


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

                priority = *(it++);
                ttl = ValueOf<uint16_t>({ it, it + 2 });
                it += 2;
                break;
            }
        }

        auto data_len_uv_size = quicr::UintVar::Size(*it);
        data_length = uint64_t(quicr::UintVar({ it, it + data_len_uv_size }));
        it += data_len_uv_size;

        data.reserve(data_length);
        data.assign(it, it + data_length);
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object)
    {
        data.push_back(static_cast<uint8_t>(data_object.type));

        switch (data_object.type) {
            case DataObjectType::kExistingStream:
                break;

            case DataObjectType::kDatagram: {
                auto sns_id_bytes = BytesOf(data_object.sns_id);
                data.insert(data.end(), sns_id_bytes.rbegin(), sns_id_bytes.rend());

                auto tfn_bytes = BytesOf(data_object.track_full_name_hash);
                data.insert(data.end(), tfn_bytes.rbegin(), tfn_bytes.rend());
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

                break;
            }
        }

        auto data_len_uv_size = quicr::UintVar(data_object.data_length);
        data.insert(data.end(), data_len_uv_size.begin(), data_len_uv_size.end());

        data.insert(data.end(), data_object.data.begin(), data_object.data.end());

        return data;
    }

    std::vector<uint8_t> DataObject::Serialize(bool include_header) const
    {
        std::vector<uint8_t> data;

        if (include_header) {
            data.reserve(kCommonHeadersSize + SizeBytes());
            data.push_back(kProtocolVersion);
            uint16_t htype = static_cast<uint8_t>(MsgType::kDataObject);
            auto type_bytes = BytesOf(htype);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto d_size = SizeBytes();
            auto data_len_bytes = BytesOf(d_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());

        } else {
            data.reserve(SizeBytes());
        }

        data << *this;
        return data;
    }
}
