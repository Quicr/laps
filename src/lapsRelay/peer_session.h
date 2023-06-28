#pragma once


#include <atomic>
#include <memory>
#include <quicr/encode.h>
#include <quicr/namespace.h>
#include <transport/transport.h>
#include <thread>
#include <transport/safe_queue.h>

#include "peer_common.h"
#include "peer_protocol.h"
#include "cache.h"
#include "client_subscriptions.h"
#include "config.h"


namespace laps {
    using namespace qtransport;
    using namespace quicr;

    /**
     * @brief Peering manager class. Manages relay to relay (peering) forwarding of
     *      subscriber objects.
     *
     */
    class PeerSession
      : public ITransport::TransportDelegate
    {
      public:
        enum class Status : uint8_t {
            CONNECTING = 0,
            CONNECTED,
            DISCONNECTED
        };

        //PeerSession(const PeerSession&) = default;
        PeerSession(const PeerSession& other)
          : peer_config { other.peer_config }
          , _config { other._config }
          , _status{ other._status.load() }
          , _peer_queue { other._peer_queue }
          , _cache { other._cache }
          , _subscriptions { other._subscriptions }
          , _peer_subscriptions { other._peer_subscriptions }
        {}

        PeerSession() = delete;
        //PeerSession(PeerSession&&) = default;
        PeerSession(const PeerSession&& other)
          : peer_config { other.peer_config }
          , _config { other._config }
          , _status{ other._status.load() }
          , _peer_queue { other._peer_queue }
          , _cache { other._cache }
          , _subscriptions { other._subscriptions }
          , _peer_subscriptions { other._peer_subscriptions }
        {}


        PeerSession& operator=(const PeerSession&) = default;
        PeerSession& operator=(PeerSession&&) = default;

        /**
         * @brief Constructor to create a new peer session
         *
         * @param is_inbound                True indicates the peering session is inbound (server accepted)
         * @param context_id                Context ID from the transport for the connection
         * @param cfg                       Global config
         * @param peer_remote               Transport remote peer config/parameters
         * @param peer_queue                Shared peer queue used by all peers
         * @param cache                     Shared cache used by all peers and clients
         * @param subscriptions             Client/edge subscriptions
         * @param peer_subscriptions        Peer subscriptions
         */
        PeerSession(const bool is_inbound,
                    const TransportContextId context_id,
                    const Config& cfg,
                    const TransportRemote& peer_remote,
                    safeQueue<PeerObject>& peer_queue,
                    Cache& cache,
                    ClientSubscriptions& subscriptions,
                    PeerSubscriptions &peer_subscriptions);

        ~PeerSession();

        /**
         * @brief Create a connection using the transport to the peer
         */
        void connect();

        /**
         * @brief Get the status of the peer session
         */
         Status status();

         /**
          * @brief Set the transport
          * @details Setting the transport is not required and should not be used for outbound connections. This
          *     Server,incoming mode requires the server transport to be used.
          */
          void setTransport(std::shared_ptr<ITransport> transport) {
              _transport = transport;
          }

        /**
         * @brief Wrapper method to transport createStream()
         *
         * @param context_id            Transport context ID
         * @param use_reliable          Indicates if datagram or streams should be used
         *
         * @return Stream Id of the newly created stream
         */
        StreamId createStream(TransportContextId context_id, bool use_reliable);

        /*
         * Delegate functions
         */
        void on_connection_status(const TransportContextId& context_id, const TransportStatus status) override;
        void on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) override;
        void on_new_stream(const TransportContextId& context_id, const StreamId& streamId) override {}
        void on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) override;

      public:
        const TransportRemote& peer_config;

      private:
        const Config& _config;
        std::atomic<Status> _status { Status::CONNECTING };    /// Status of this peer session

        peerQueue& _peer_queue;
        Cache& _cache;
        ClientSubscriptions& _subscriptions;
        PeerSubscriptions& _peer_subscriptions;
        Logger* logger;

        bool _is_inbound { false };               /// Indicates if the peer is server accepted (inbound) or client (outbound)

        /*
         * Information about peer
         */
        std::string peer_id;                      /// ID/Name of the peer
        double  longitude { 0 };                  /// 8 byte longitude value detailing the location of the local relay
        double  latitude { 0 };                   /// 8 byte latitude value detailing the location of the local relay

        /// Initial list of namespaces to subscribe to on connection established
        nameList subscribe_namespaces;

        TransportConfig _transport_config {
            .tls_cert_filename = _config.tls_cert_filename.c_str(),
            .tls_key_filename = _config.tls_key_filename.c_str(),
            .time_queue_init_queue_size = _config.data_queue_size,
            .time_queue_max_duration = 1000,
            .time_queue_bucket_interval = 2,
            .time_queue_rx_ttl = _config.time_queue_ttl_default
        };

        TransportContextId t_context_id;         /// Transport context ID
        StreamId           dgram_stream_id;      /// Datagram stream ID
        StreamId           control_stream_id;    /// Control stream ID

        std::shared_ptr<ITransport> _transport;   /// Transport used for the peering connection

    };

} // namespace laps