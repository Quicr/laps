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
        uint32_t size = 1 /* type */
        + quicr::UintVar(group_id).Size()
        + quicr::UintVar(sub_group_id).Size()
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

    bool DataObject::Deserialize(Span<const uint8_t> serialized_data)
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

                auto group_id_size = quicr::UintVar::Size(*it);
                group_id = uint64_t(quicr::UintVar({ it, it + group_id_size }));
                it += group_id_size;

                auto subgroup_id_size = quicr::UintVar::Size(*it);
                sub_group_id = uint64_t(quicr::UintVar({ it, it + subgroup_id_size }));
                it += subgroup_id_size;

                break;
            }

            case DataObjectType::kNewStream: {
                sns_id = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;

                track_full_name_hash = ValueOf<uint64_t>({ it, it + 8 });
                it += 8;

                auto group_id_size = quicr::UintVar::Size(*it);
                group_id = uint64_t(quicr::UintVar({ it, it + group_id_size }));
                it += group_id_size;

                auto subgroup_id_size = quicr::UintVar::Size(*it);
                group_id = uint64_t(quicr::UintVar({ it, it + subgroup_id_size }));
                it += subgroup_id_size;

                priority = *(it++);
                ttl = ValueOf<uint32_t>({ it, it + 4 });
                it += 4;


                break;
            }
        }

        auto data_len_uv_size = quicr::UintVar::Size(*it);
        data_length = uint64_t(quicr::UintVar({ it, it + data_len_uv_size }));
        it += data_len_uv_size;

        if (data_len_uv_size <= serialized_data.end() - it) {
            data = {it, it + data_length};
            return true;

        }

        if (data_length >= 2'000'000) {
            SPDLOG_WARN("Received data object is very large size: {}", data_length);
        }

        data_storage.reserve(data_length);
        data_storage.insert(data_storage.end(), it, serialized_data.end());

        data = {data_storage.begin(), data_storage.end()};

        return false;
    }

    std::pair<uint64_t, bool> DataObject::AppendData(Span<const uint8_t> a_data)
    {
        bool is_complete{ false };
        uint64_t read_bytes{ 0 };

        const auto remaining_data_bytes = data_length - data.size();


        data_storage.insert(data_storage.end(), a_data.begin(), a_data.end());
        data = { data_storage.begin(), data_storage.end() };

        return {read_bytes, is_complete};
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

                auto group_id_bytes = quicr::UintVar(data_object.group_id);
                data.insert(data.end(), group_id_bytes.begin(), group_id_bytes.end());

                auto subgroup_id_bytes = quicr::UintVar(data_object.sub_group_id);
                data.insert(data.end(), subgroup_id_bytes.begin(), subgroup_id_bytes.end());

                break;
            }

            case DataObjectType::kNewStream: {
                auto sns_id_bytes = BytesOf(data_object.sns_id);
                data.insert(data.end(), sns_id_bytes.rbegin(), sns_id_bytes.rend());

                auto tfn_bytes = BytesOf(data_object.track_full_name_hash);
                data.insert(data.end(), tfn_bytes.rbegin(), tfn_bytes.rend());

                auto group_id_bytes = quicr::UintVar(data_object.group_id);
                data.insert(data.end(), group_id_bytes.begin(), group_id_bytes.end());

                auto subgroup_id_bytes = quicr::UintVar(data_object.sub_group_id);
                data.insert(data.end(), subgroup_id_bytes.begin(), subgroup_id_bytes.end());

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

    std::vector<uint8_t> DataObject::Serialize()
    {
        std::vector<uint8_t> net_data;

        data_length = data.size();

        net_data.reserve(SizeBytes());

        net_data << *this;
        return net_data;
    }
}
