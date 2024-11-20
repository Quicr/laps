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

    uint32_t DataObject::SizeBytes(bool include_header, DataObjectType type) const
    {
        uint32_t size = quicr::UintVar(data.size()).Size() + data.size();

        if (include_header) {
            size += sizeof(sns_id) + sizeof(track_full_name_hash);

            switch (type) {
                case DataObjectType::kDatagram:
                    break;

                case DataObjectType::kExistingStream:
                    break;

                case DataObjectType::kNewStream:
                    size += sizeof(priority) + sizeof(ttl);
                    break;
            }
        }

        return size;
    }

    DataObject::DataObject(Span<const uint8_t> serialized_data)
    {
        auto it = serialized_data.begin();

        type = static_cast<DataObjectType>(*(it++));

        sns_id = ValueOf<uint32_t>({ it, it + 4 });
        it += 4;

        track_full_name_hash = ValueOf<uint64_t>({ it, it + 8 });
        it += 8;

        switch (type) {
            case DataObjectType::kDatagram:
                break;
            case DataObjectType::kExistingStream:
                break;
            case DataObjectType::kNewStream:
                priority = *(it++);
                ttl = ValueOf<uint16_t>({ it, it + 2 });
                it += 2;
                break;
        }

        auto data_len_uv_size = quicr::UintVar::Size(*it);
        data_length = uint64_t(quicr::UintVar({ it, it + data_len_uv_size }));
        it += data_len_uv_size;

        data.reserve(data_length);
        data.assign(it, it + data_length);
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const DataObject& data_object)
    {
        auto data_len_uv_size = quicr::UintVar(data_object.data_length);
        data.insert(data.end(), data_len_uv_size.begin(), data_len_uv_size.end());

        // TODO(tievens): Revisit copying of data objects
        data.insert(data.end(), data_object.data.begin(), data_object.data.end());

        return data;
    }

    std::vector<uint8_t> DataObject::Serialize(bool include_header, DataObjectType data_type) const
    {
        std::vector<uint8_t> data;

        if (include_header) {
            data.reserve(kCommonHeadersSize + SizeBytes(true, data_type));
            data.push_back(kProtocolVersion);
            uint16_t type = static_cast<uint8_t>(MsgType::kDataObject);
            auto type_bytes = BytesOf(type);
            data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
            auto ni_size = SizeBytes(true, data_type);
            auto data_len_bytes = BytesOf(ni_size);
            data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());


            switch (data_type) {
                case DataObjectType::kDatagram:
                    break;
                case DataObjectType::kExistingStream:
                    break;
                case DataObjectType::kNewStream:
                    break;
            }

        } else {
            data.reserve(SizeBytes(false, data_type));
        }

        data << *this;
        return data;
    }
}
