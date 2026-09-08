// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "config.h"

#include <doctest/doctest.h>

TEST_CASE("Transport backend selection")
{
    CHECK(laps::kDefaultTransportBackendName == "msquic");
    CHECK(laps::kDefaultTransportBackend == quicr::TransportBackend::kMsQuic);
    CHECK(laps::ParseTransportBackend("msquic") == quicr::TransportBackend::kMsQuic);
    CHECK(laps::ParseTransportBackend("picoquic") == quicr::TransportBackend::kPicoQuic);
    CHECK_FALSE(laps::ParseTransportBackend("unknown").has_value());
}
