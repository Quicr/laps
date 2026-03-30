#include "moxygen_client_manager.h"

#include <string>
#include <vector>

namespace laps {
    MoxygenClientManager::MoxygenClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
      : ClientManager(state, config, peer_manager)
      , moxygen::MoQServer(quic::samples::createFizzServerContext(
                             [] {
                                 std::vector<std::string> alpns = { "h3" };
                                 auto moqt = moxygen::getMoqtProtocols("16", true);
                                 alpns.insert(alpns.end(), moqt.begin(), moqt.end());
                                 return alpns;
                             }(),
                             fizz::server::ClientAuthMode::Optional,
                             config.tls_cert_filename_,
                             config.tls_key_filename_),
                           "/moxygen-relay")
      , config_(config)
    {
    }

    MoxygenClientManager::~MoxygenClientManager()
    {
        stop();
    }

    bool MoxygenClientManager::Start()
    {
        folly::SocketAddress addr("::", config_.port);
        start(addr);

        return true;
    }

    void MoxygenClientManager::onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession)
    {
        clientSession->setPublishHandler(relay_);
        clientSession->setSubscribeHandler(relay_);
    }

    std::shared_ptr<moxygen::MoQSession> MoxygenClientManager::createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor)
    {
        return std::make_shared<moxygen::MoQRelaySession>(
          folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)), *this, std::move(executor));
    }
}