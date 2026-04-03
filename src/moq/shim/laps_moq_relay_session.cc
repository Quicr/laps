// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "laps_moq_relay_session.h"

#include <spdlog/spdlog.h>

namespace laps {

    void LapsMoqRelaySession::SendPublishNamespaceAck(moxygen::RequestID request_id)
    {
        moxygen::PublishNamespaceOk ann_ok{};
        ann_ok.requestID = request_id;

        const auto res = moqFrameWriter_.writePublishNamespaceOk(controlWriteBuf_, ann_ok);
        if (!res) {
            SPDLOG_WARN("LapsMoqRelaySession::SendPublishNamespaceAck: writePublishNamespaceOk failed");
            return;
        }
        controlWriteEvent_.signal();
    }

} // namespace laps
