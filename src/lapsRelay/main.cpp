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

namespace {
std::pair<quicr::RelayInfo, qtransport::TransportConfig>
make_server_configs(const Config& cfg)
{
    quicr::RelayInfo relay_info = {
        .hostname = cfg.client_config.bind_addr,
        .port = cfg.client_config.listen_port,
        .proto = cfg.client_config.protocol,
    };

    qtransport::TransportConfig tcfg{
        .tls_cert_filename = cfg.tls_cert_filename.empty() ? nullptr : cfg.tls_cert_filename.c_str(),
        .tls_key_filename = cfg.tls_key_filename.empty() ? nullptr : cfg.tls_key_filename.c_str(),
        .time_queue_init_queue_size = cfg.data_queue_size,
        .debug = false,
        .quic_cwin_minimum = static_cast<uint64_t>(cfg.cwin_min_kb * 1024),
    };

    return { relay_info, tcfg };
}
}

int
main(int /* argc */, char*[] /* argv[] */)
{
    Config cfg;
    logger = std::make_shared<cantina::Logger>("MAIN", cfg.logger);

    FLOG_INFO("Starting LAPS Relay (version " << lapsVersion() << ")");

    if (cfg.disable_dedup) {
        logger->info << "Disable dedup of objects" << std::flush;
    }

    if (cfg.disable_splithz) {
        logger->info << "Disable split horizon" << std::flush;
    }

    Cache cache(cfg);
    peerQueue peer_queue;

    {
        // Start UDP client manager
        auto [udp_relay_info, udp_tcfg] = make_server_configs(cfg);
        ClientSubscriptions udp_subscriptions{ cfg };
        auto udp_mgr = ClientManager::create(cfg, cache, udp_subscriptions, peer_queue);
        auto udp_server = std::make_shared<quicr::Server>(udp_relay_info, std::move(udp_tcfg), udp_mgr, logger);
        ForwardedServer udp_forward_server{
            udp_server,
            udp_mgr,
        };

        // Set QUIC client manager to use the UDP port plus one
        {
            cfg.client_config.listen_port++;
            if (cfg.tls_cert_filename.empty()) {
                cfg.tls_cert_filename = "./server-cert.pem";
                cfg.tls_key_filename = "./server-key.pem";
            }
            cfg.client_config.protocol = RelayInfo::Protocol::QUIC;
        }

        auto [quic_relay_info, quic_tcfg] = make_server_configs(cfg);
        ClientSubscriptions quic_subscriptions{ cfg };
        auto quic_mgr = ClientManager::create(cfg, cache, quic_subscriptions, peer_queue);
        auto quic_server = std::make_shared<quicr::Server>(quic_relay_info, std::move(quic_tcfg), quic_mgr, logger);
        ForwardedServer quic_forward_server{
            quic_server,
            quic_mgr,
        };

        // Set peering config
        {
            cfg.tls_cert_filename = {};
            cfg.tls_key_filename = {};
            cfg.peer_config.protocol = RelayInfo::Protocol::QUIC;
        }
        PeerManager peer_mgr(cfg, peer_queue, cache, { udp_forward_server, quic_forward_server });

        udp_server->run();
        quic_server->run();

        while (quic_server->is_transport_ready() && udp_server->is_transport_ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }

    FLOG_INFO("LAPS stopped");
}
