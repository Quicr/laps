// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "relay.h"

#include "client_manager.h"

#include <quicr/config.h>
#include <quicr/session.h>

#include <stdexcept>
#include <utility>

namespace laps {
    namespace {
        moq::fabric::Config PeeringConfig(const Config& config)
        {
            moq::fabric::Config peering;
            peering.node_id = config.relay_id_;
            peering.node_type = config.node_type;
            peering.listen_port = config.peering.listening_port;
            peering.peers = config.peering.peers;
            peering.tls_cert_filename = config.tls_cert_filename_;
            peering.tls_key_filename = config.tls_key_filename_;
            peering.object_ttl_ms = config.object_ttl_;
            peering.check_interval_ms = config.peering.check_interval_ms;
            peering.init_queue_size = config.peering.init_queue_size;
            peering.debug = config.debug;
            peering.logger = config.logger_;
            return peering;
        }

        quicr::ServerConfig ServerConfig(const Config& config)
        {
            quicr::ServerConfig server;
            server.endpoint_id = config.relay_id_;
            server.server_bind_ip = config.bind_ip;
            server.server_port = config.port;
            server.transport_config.debug = config.debug;
            server.transport_config.tls_cert_filename = config.tls_cert_filename_;
            server.transport_config.tls_key_filename = config.tls_key_filename_;
            server.transport_config.quic_qlog_path = config.qlog_path_;
            server.transport_config.idle_timeout_ms = 10000;
            server.transport_config.time_queue_rx_size = 10'000;
            server.transport_config.time_queue_bucket_interval = 500;
            server.transport_config.time_queue_max_duration = config.object_ttl_ * 2;
            server.transport_config.max_connections = 5000;
            return server;
        }
    }

    Relay::Relay(Config config)
      : config_(std::move(config))
      , peer_manager_(PeeringConfig(config_))
      , client_manager_(std::make_shared<ClientManager>(state_,
                                                        config_,
                                                        ServerConfig(config_),
                                                        peer_manager_,
                                                        config_.cache_duration_ms))
    {
        peer_manager_.SetDelegate(client_manager_.get());

        if (client_manager_->Start() != quicr::Session::Status::kReady) {
            client_manager_->Stop();
            throw std::runtime_error("Relay failed to start");
        }
    }

    Relay::~Relay()
    {
        client_manager_->Stop();
    }
}
