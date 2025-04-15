// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "connect_response.h"

namespace laps::peering {

    constexpr uint16_t kConnectResponseType = static_cast<uint16_t>(MsgType::kConnectResponse);

    uint32_t ConnectResponse::SizeBytes() const
    {
        return error == ProtocolError::kNoError ? sizeof(error) + node_info->SizeBytes() : sizeof(error);
    }

    ConnectResponse::ConnectResponse(std::span<uint8_t const> serialized_data)
    {
        auto it = serialized_data.begin();

        error = static_cast<ProtocolError>(ValueOf<uint16_t>({ it, it + 2 }));
        it += 2;

        if (error == ProtocolError::kNoError) {
            std::span<const uint8_t> data(it, serialized_data.end());
            node_info = NodeInfo(data);
        }
    }

    std::vector<uint8_t>& operator<<(std::vector<uint8_t>& data, const ConnectResponse& connect_resp)
    {
        // Get the size in bytes to be written
        auto size = connect_resp.SizeBytes();

        // Common header
        data.push_back(kProtocolVersion);

        auto type_bytes = BytesOf(kConnectResponseType);
        data.insert(data.end(), type_bytes.rbegin(), type_bytes.rend());
        auto data_len_bytes = BytesOf(size);
        data.insert(data.end(), data_len_bytes.rbegin(), data_len_bytes.rend());

        auto error_code = static_cast<uint16_t>(connect_resp.error);
        auto error_bytes = BytesOf(error_code);
        data.insert(data.end(), error_bytes.rbegin(), error_bytes.rend());

        if (connect_resp.error == ProtocolError::kNoError) {
            if (not connect_resp.node_info.has_value()) {
                throw std::runtime_error("Invalid connect response; missing node info");
            }

            auto ni_data = connect_resp.node_info->Serialize();
            data.insert(data.end(), ni_data.begin(), ni_data.end());
        }

        return data;
    }

    std::vector<uint8_t> ConnectResponse::Serialize() const
    {
        std::vector<uint8_t> data;
        data.reserve(kCommonHeadersSize + SizeBytes());
        data << *this;
        return data;
    }
}
