#pragma once


#include <atomic>
#include <memory>
#include <unordered_set>
#include <quicr/encode.h>
#include <map>
#include <quicr/namespace.h>
#include <transport/transport.h>
#include <thread>
#include <transport/safe_queue.h>

#include "peer_common.h"
#include "peer_protocol.h"
#include "peer_session.h"
#include "cache.h"
#include "client_subscriptions.h"
#include "config.h"


namespace laps {
    using namespace qtransport;
    using namespace quicr;

    /**
     * @brief Peering manager class. Manages relay to relay (peering) forwarding of
     *      subscriber objects.
     */
    class PeerManager
      : public ITransport::TransportDelegate
    {
      public:

        PeerManager(const Config& cfg,
                    safeQueue<PeerObject>& peer_queue,
                    Cache& cache,
                    ClientSubscriptions& subscriptions);

        ~PeerManager();

        /*
         * Delegate functions for Incoming (e.g., server side)
         */
        void on_connection_status(const TransportContextId& context_id, const TransportStatus status) override;
        void on_new_connection(const TransportContextId& context_id, const TransportRemote& remote) override;
        void on_new_stream(const TransportContextId& context_id, const StreamId& streamId) override {}
        void on_recv_notify(const TransportContextId& context_id, const StreamId& streamId) override;

      private:


        /**
         * @brief Client thread to monitor client published messages
         */
        void ClientRxThread();

        /**
         * @brief Watch Thread to perform reconnects and cleanup
         * @details Thread will perform various tasks each interval
         *
         * @param interval_ms       Interval in milliseconds to sleep before running check
         */
        void watchThread(int interval_ms);


        /**
         * @brief Create a peering session/connection
         *
         * @param peer_config           Peer/relay configuration parameters
         *
         */
        void createPeerSession(const TransportRemote& peer_config);

      private:
        std::atomic<bool> _stop{ false };
        const Config& _config;

        peerQueue& _peer_queue;
        Cache& _cache;
        ClientSubscriptions& _subscriptions;

        std::shared_ptr<ITransport> _server_transport;           /// Server Transport for inbound connections


        std::thread _client_rx_msg_thr;                         /// Client receive message thread
        std::thread _watch_thr;                                 /// Watch/task thread, handles reconnects


        /// Peer sessions that are accepted by the server
        std::map<TransportContextId , PeerSession> _server_peer_sessions;


        std::vector<PeerSession> _client_peer_sessions;          /// Peer sessions that are initiated by the peer manager

        // Log handler to use
        Logger* logger;
    };

} // namespace laps