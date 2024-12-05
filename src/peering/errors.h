// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "common.h"

namespace laps::peering {

    enum class ProtocolError : uint16_t
    {
        kNoError = 0,
        kConnectError,         ///< General connect error
        kConnectNotAuthorized, ///< Connect is not authorized
    };
} // namespace laps