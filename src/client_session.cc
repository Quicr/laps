// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "client_session.h"
#include "client_manager.h"

namespace laps {

    void ClientSession::MetricsSampled(const quicr::ConnectionMetrics& metrics)
    {
        manager_.MetricsSampled(GetConnectionHandle(), metrics);
    }

} // namespace laps
