// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "peering/common.h"
#include "peering/errors.h"
#include <optional>

#include "node_info.h"

namespace laps::peering {

    /**
     * @brief Peering Connect response message
     *
     * @details Peering connect response message is sent by the the server side of the peer in response
     *    to a connect message.
     */
    class ConnectResponse : public CommonHeaders
    {
      public:
        ProtocolError error{
            ProtocolError::kNoError
        }; ///< NoError or error value for connect. If error, node_info is not set

        std::optional<NodeInfo> node_info; ///< This node information of server accepting the connection

        /*
         * Parameters
         */

        /**
         * @brief Serialize the message into wire format to be transmitted
         */
        std::vector<uint8_t> Serialize() const;

        ConnectResponse() = default;
        ConnectResponse(Span<uint8_t const> serialized_data);

        uint32_t SizeBytes() const;

      private:
    };

} // namespace laps