#include "client_manager.h"

#if defined(LAPS_HAVE_DUAL_MOQ_BACKENDS)
#include "moxygen_client_manager.h"
#include "quicr_client_manager.h"
#elif defined(WITH_QUICR)
#include "quicr_client_manager.h"
#elif defined(WITH_MOXYGEN)
#include "moxygen_client_manager.h"
#endif

laps::ClientManager::ClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
  : state_(state)
  , config_(config)
  , peer_manager_(peer_manager)
  , cache_duration_ms_(config.cache_duration_ms)
{
}

void laps::ClientManager::ProcessSubscribe(quicr::ConnectionHandle,
                                           uint64_t,
                                           const quicr::TrackHash&,
                                           const quicr::FullTrackName&,
                                           const quicr::messages::SubscribeAttributes&,
                                           std::optional<quicr::messages::Location>)
{
}

void laps::ClientManager::RemoveOrPausePublisherSubscribe(quicr::TrackFullNameHash)
{
}

void laps::ClientManager::ApplyPeerAnnouncePublishNamespace(quicr::ConnectionHandle,
                                                            const quicr::TrackNamespace&,
                                                            const quicr::PublishNamespaceAttributes&)
{
}

void laps::ClientManager::ApplyPeerAnnouncePublishNamespaceDone(quicr::ConnectionHandle,
                                                                quicr::messages::RequestID)
{
}

void laps::ClientManager::ApplyPeerAnnouncePublish(quicr::ConnectionHandle,
                                                   uint64_t,
                                                   const quicr::messages::PublishAttributes&,
                                                   std::weak_ptr<quicr::SubscribeNamespaceHandler>)
{
}

void laps::ClientManager::RelayBindPublisherTrack(quicr::ConnectionHandle,
                                                 quicr::ConnectionHandle,
                                                 uint64_t,
                                                 const std::shared_ptr<quicr::PublishTrackHandler>&,
                                                 bool)
{
}

void laps::ClientManager::RelayUnbindPublisherTrack(quicr::ConnectionHandle,
                                                    quicr::ConnectionHandle,
                                                    const std::shared_ptr<quicr::PublishTrackHandler>&,
                                                    bool)
{
}

std::optional<quicr::PublishTrackHandler::PublishObjectStatus> laps::ClientManager::TryMoxygenRelayPublishObject(
  quicr::PublishTrackHandler*,
  quicr::TrackMode,
  const quicr::ObjectHeaders&,
  quicr::BytesSpan)
{
    return std::nullopt;
}

std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
laps::ClientManager::TryMoxygenRelayForwardPublishedData(quicr::PublishTrackHandler*,
                                                        bool,
                                                        uint64_t,
                                                        uint64_t,
                                                        std::shared_ptr<const std::vector<uint8_t>>)
{
    return std::nullopt;
}

std::optional<quicr::PublishTrackHandler::PublishObjectStatus> laps::ClientManager::TryMoxygenFetchPublishObject(
  quicr::PublishFetchHandler*,
  const quicr::ObjectHeaders&,
  quicr::BytesSpan)
{
    return std::nullopt;
}

std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
laps::ClientManager::TryMoxygenFetchForwardPublishedData(quicr::PublishFetchHandler*,
                                                       bool,
                                                       uint64_t,
                                                       uint64_t,
                                                       std::shared_ptr<const std::vector<uint8_t>>)
{
    return std::nullopt;
}

std::shared_ptr<laps::ClientManager>
laps::MakeClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
{
#if defined(LAPS_HAVE_DUAL_MOQ_BACKENDS)
    if (config.moq_transport_backend == MoqTransportBackend::kLibquicr) {
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
    }
    return std::make_shared<MoxygenClientManager>(state, config, peer_manager);
#elif defined(WITH_QUICR)
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
    return std::make_shared<MoxygenClientManager>(state, config, peer_manager);
#else
#error "Backend unsupported"
#endif
}
