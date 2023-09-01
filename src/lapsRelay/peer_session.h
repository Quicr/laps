#pragma once


#include <atomic>
#include <memory>
#include <quicr/encode.h>
#include <quicr/namespace.h>
#include <quicr/message_buffer.h>
#include <transport/transport.h>
#include <thread>
#include <transport/safe_queue.h>

#include <cantina/logger.h>

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
          , logger { other.logger }

        {
            _transport = std::move(other._transport);
            peer_id = peer_config.host_or_ip;
        }

        PeerSession() = delete;

        //PeerSession(PeerSession&&) = default;
        PeerSession(const PeerSession&& other)
          : peer_config { other.peer_config }
          , _config { other._config }
          , _status{ other._status.load() }
          , _peer_queue { other._peer_queue }
          , _cache { other._cache }
          , _subscriptions { other._subscriptions }
          , logger { other.logger }
        {
            _transport = std::move(other._transport);
            peer_id = peer_config.host_or_ip;
        }

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
         */
        PeerSession(const bool is_inbound,
                    const TransportContextId context_id,
                    const Config& cfg,
                    const TransportRemote& peer_remote,
                    safe_queue<PeerObject>& peer_queue,
                    Cache& cache,
                    ClientSubscriptions& subscriptions);

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
          * @brief Get the peer ID of session
          */
          const peer_id_t & getPeerId() {
              return peer_id;
          }

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

        /**
         * @brief Send/publish object to peers
         *
         * @param obj          Published Object to send
         */
        void publishObject(const messages::PublishDatagram& obj);

        /**
         * @brief Send subscribe to peer
         *
         * TODO: Currently this is to all peers, we will likely want to target specific peers
         *
         * @param ns        Namespace to subscribe
         */
        void sendSubscribe(const Namespace& ns);


        /**
         * @brief Send unsubscribe to peer
         *
         * TODO: Currently this is to all peers, we will likely want to target specific peers
         *
         * @param ns        Namespace to subscribe
         */
         void sendUnsubscribe(const Namespace& ns);

         void sendPublishIntent(const Namespace& ns, const peer_id_t& origin_peer_id);
         void sendPublishIntentDone(const Namespace& ns, const peer_id_t& origin_peer_id);

        /*
         * Delegate functions mainly for Outgoing but does include incoming
         */
        void on_connection_status(const TransportContextId& context_id, const TransportStatus status) override;
        void on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) override;
        void on_new_stream(const TransportContextId& context_id, const StreamId& streamId) override {}
        void on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) override;

      private:
        void sendConnect();
        void sendConnectOk();

        void addSubscription(const Namespace& ns);


      public:
        TransportRemote peer_config;

      private:
        const Config& _config;
        std::atomic<Status> _status { Status::CONNECTING };    /// Status of this peer session

        peerQueue& _peer_queue;
        Cache& _cache;
        ClientSubscriptions& _subscriptions;
        cantina::LoggerPointer logger;

        bool _is_inbound { false };               /// Indicates if the peer is server accepted (inbound) or client (outbound)
        bool _use_reliable { true };              /// Indicates if to use reliable/streams or datagram when publishing objects

        /*
         * Information about peer
         */
        peer_id_t peer_id;                        /// ID/Name of the peer
        double  longitude { 0 };                  /// 8 byte longitude value detailing the location of the local relay
        double  latitude { 0 };                   /// 8 byte latitude value detailing the location of the local relay

        TransportConfig _transport_config {
            .tls_cert_filename = _config.tls_cert_filename.c_str(),
            .tls_key_filename = _config.tls_key_filename.c_str(),
            .time_queue_init_queue_size = _config.data_queue_size,
            .time_queue_max_duration = 1000,
            .time_queue_bucket_interval = 2,
            .time_queue_size_rx = _config.rx_queue_size
        };

        TransportContextId t_context_id;              /// Transport context ID
        StreamId           dgram_stream_id;           /// Datagram stream ID
        StreamId           control_stream_id;         /// Control stream ID

        std::shared_ptr<ITransport> _transport;       /// Transport used for the peering connection

        namespace_map<StreamId> _subscribed;          /// Subscribed namespace and associated stream id
        namespace_map<peer_id_t> _publish_intents;    /// Publish intents sent to the peer
    };

} // namespace laps
