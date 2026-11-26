// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "config.h"
#include "state.h"

#include <moq/fabric/peer_manager.h>

#include <memory>

namespace laps {
    class ClientManager;

    // LAPS relay instance.
    class Relay
    {
      public:
        /**
         * Create and start a LAPS relay.
         * @throws std::runtime_error if the client transport fails to start
         */
        explicit Relay(Config config);

        ~Relay();

        Relay(const Relay&) = delete;
        Relay& operator=(const Relay&) = delete;

      private:
        const Config config_;
        State state_;
        moq::fabric::PeerManager peer_manager_;
        const std::shared_ptr<ClientManager> client_manager_;
    };
}
