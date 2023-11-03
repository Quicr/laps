#include <chrono>
#include <sstream>

#include "cache.h"
#include "client_manager.h"
#include "peer_common.h"
#include "peer_manager.h"

#include "config.h"
#include "version_config.h"
#include <cantina/logger.h>
#include "logger.h"

using namespace laps;
using namespace qtransport;
using namespace quicr;

static cantina::LoggerPointer logger;

std::string
lapsVersion()
{
    return LAPS_VERSION;
}

int
main(int argc,  char* argv[])
{
    Config cfg;

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg.compare("-h") == 0 || arg.compare("help") == 0) {
            cfg.print_help();
            return 0;
        }
    }

    logger = std::make_shared<cantina::Logger>("MAIN", cfg.logger);

    FLOG_INFO("Starting LAPS Relay (version " << lapsVersion() << ")");

    if (cfg.disable_dedup) {
        logger->info << "Disable dedup of objects" << std::flush;
    }

    if (cfg.disable_splithz) {
        logger->info << "Disable split horizon" << std::flush;
    }

    ClientSubscriptions subscriptions(cfg);
    Cache cache(cfg);
    peerQueue peer_queue;

    {
        // Start UDP client manager
        Config cfg_udp = cfg;

        auto udp_mgr = ClientManager::create(cfg_udp, cache, subscriptions, peer_queue);
        udp_mgr->start();

        cfg.peer_config.protocol = RelayInfo::Protocol::QUIC;
        PeerManager peer_mgr(cfg, peer_queue, cache, subscriptions);

        // Start QUIC client manager using the UDP port plus one
        cfg.client_config.listen_port++;

        if (cfg.tls_cert_filename.empty()) {
            cfg.tls_cert_filename = "./server-cert.pem";
            cfg.tls_key_filename = "./server-key.pem";
        }

        cfg.client_config.protocol = RelayInfo::Protocol::QUIC;

        Config cfg_quic = cfg;
        auto quic_mgr = ClientManager::create(cfg_quic, cache, subscriptions, peer_queue);
        quic_mgr->start();

        while (quic_mgr->ready() && udp_mgr->ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }

        udp_mgr->stop();
        quic_mgr->stop();
    }

    FLOG_INFO("LAPS stopped");
}
