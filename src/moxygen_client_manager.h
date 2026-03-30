#pragma once

#include "client_manager.h"

#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQServer.h>
#include <moxygen/relay/MoQRelay.h>

namespace laps {
    class MoxygenClientManager
      : public ClientManager
      , public moxygen::MoQServer
    {
      public:
        MoxygenClientManager(State& state, const Config& config, peering::PeerManager& peer_manager);

        virtual ~MoxygenClientManager();

        bool Start() override;

        void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

      protected:
        std::shared_ptr<moxygen::MoQSession> createSession(folly::MaybeManagedPtr<proxygen::WebTransport> wt,
                                                           std::shared_ptr<moxygen::MoQExecutor> executor) override;

      private:
        std::shared_ptr<moxygen::MoQRelay> relay_{ std::make_shared<moxygen::MoQRelay>(100, 3) };
        const Config& config_;
    };
}