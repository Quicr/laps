#include "client_manager.h"

#ifdef WITH_QUICR
#include "quicr_client_manager.h"
#elif defined(WITH_MOXYGEN)
#include "moxygen_client_manager.h"
#endif

laps::ClientManager::ClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
  : state_(state)
  , config_(config)
  , peer_manager_(peer_manager)
{
    peer_manager_.SetClientManager(shared_from_this());
}

std::shared_ptr<laps::ClientManager>
laps::MakeClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
{
#ifdef WITH_QUICR
    quicr::ServerConfig cfg;

    cfg.endpoint_id = config.relay_id_;
    cfg.server_bind_ip = config.bind_ip;
    cfg.server_port = config.port;

    cfg.transport_config.debug = config.debug;
    cfg.transport_config.tls_cert_filename = config.tls_cert_filename_;
    cfg.transport_config.tls_key_filename = config.tls_key_filename_;
    cfg.transport_config.use_reset_wait_strategy = false;
    cfg.transport_config.quic_qlog_path = config.qlog_path_;
    cfg.transport_config.idle_timeout_ms = 10000;
    cfg.transport_config.time_queue_rx_size = 10'000;
    cfg.transport_config.time_queue_max_duration = config.object_ttl_ * 2;
    cfg.transport_config.max_connections = 5000;

    return std::make_shared<QuicrClientManager>(state, config, cfg, peer_manager);
#elif defined(WITH_MOXYGEN)
#error "Moxygen Unimplemented"
#else
#error "Backend unsupported"
#endif
}
