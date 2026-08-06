// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "config.h"

#include <quicr/containers/safe_queue.h>
#include <quicr/metrics.h>
#include <quicr/session.h>
#include <quicr/track_name.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace laps {
    class ClientManager;

    /**
     * @brief Publishes relay metrics samples as JSON lines over MoQ tracks.
     *
     * @details Metrics samples arrive via Queue*Metrics(), always called from MetricsSampled() callbacks on
     *      the relay's transport thread. That call must stay cheap (just an enqueue) so it never delays the
     *      transport thread. A dedicated worker thread periodically (every kBatchIntervalMs) drains the
     *      queue and coalesces the pending JSON lines into a single batch -- pure CPU work that never
     *      touches ClientManager or libquicr handler state, so it's safe to run off the transport thread.
     *
     *      Actually publishing a batch (EnsureMetricsTracks()/PublishLocalObject(), which read/write
     *      ClientManager's state_ maps and libquicr handler objects such as PublishTrackHandler's internal
     *      stream state) must still happen only on the transport thread: those structures are unsynchronized
     *      and are mutated directly by libquicr (e.g. Session::BindPublisherTrack()) with no locking. So the
     *      worker thread only stages a finished batch; the next Queue*Metrics() call (on the transport
     *      thread) picks it up and publishes it. This keeps the expensive/blocking work (batching) off the
     *      transport thread while keeping every unsafe-to-share call on the one thread that's allowed to
     *      make it.
     */
    class MetricsPublisher
    {
      public:
        MetricsPublisher(ClientManager& server, const Config& config);
        ~MetricsPublisher();

        MetricsPublisher(const MetricsPublisher&) = delete;
        MetricsPublisher& operator=(const MetricsPublisher&) = delete;

        void Start();
        void Stop();

        void AddConnection(std::uint64_t connection_handle, const quicr::Session::ConnectionRemoteInfo& remote);
        void SetConnectionEndpointId(std::uint64_t connection_handle, const std::string& endpoint_id);
        void RemoveConnection(std::uint64_t connection_handle);

        void QueueConnectionMetrics(std::uint64_t connection_handle,
                                    std::size_t publish_track_count,
                                    const quicr::ConnectionMetrics& metrics);
        void QueuePublishMetrics(std::uint64_t connection_handle,
                                 const quicr::FullTrackName& track_name,
                                 std::size_t subscriber_count,
                                 const quicr::PublishTrackMetrics& metrics);
        void QueueSubscribeMetrics(std::uint64_t connection_handle,
                                   const quicr::FullTrackName& track_name,
                                   const quicr::SubscribeTrackMetrics& metrics);

      private:
        enum class MetricType : std::uint8_t
        {
            kConnection,
            kSubscribe,
            kPublish,
        };

        struct MetricsSample
        {
            std::string json_line;
        };

        struct TrackState
        {
            quicr::FullTrackName full_track_name;
            std::uint64_t track_fullname_hash{ 0 };
            std::uint64_t next_group_id{ 0 };
        };

        struct RemoteInfo
        {
            std::string ip;
            std::uint16_t port{ 0 };

            /// Remote MoQ endpoint ID from CLIENT_SETUP. Empty until setup is received.
            std::string endpoint_id;
        };

        static constexpr std::uint32_t kQueueLimit = 5'000;
        static constexpr std::chrono::milliseconds kBatchInterval{ 150 };

        void WorkerRun();
        void FlushPendingBatch();
        void PublishSample(const MetricsSample& sample);
        void EnsureMetricsTracks();
        quicr::FullTrackName MetricsTrackName() const;
        std::string MetricsNamespaceStr() const;
        static const char* TypeName(MetricType type);

        RemoteInfo LookupRemote(std::uint64_t connection_handle);

        bool QueueSample(MetricsSample sample);

        ClientManager& server_;
        const Config& config_;
        std::vector<std::string> metrics_namespace_entries_;

        quicr::SafeQueue<MetricsSample> queue_{ kQueueLimit };
        std::atomic_bool running_{ false };

        /// Batches JSON lines pulled off queue_ on its own schedule; never touches ClientManager/libquicr.
        std::thread worker_;
        std::mutex worker_mutex_;
        std::condition_variable worker_cv_;

        /// Hand-off slot: worker_ stages a finished batch here; the transport thread (via QueueSample()) is
        /// the only thread that consumes it, since publishing must happen there.
        std::mutex pending_batch_mutex_;
        std::optional<MetricsSample> pending_batch_;

        std::mutex tracks_mutex_;
        std::optional<TrackState> metrics_track_;
        std::map<std::uint64_t, RemoteInfo> remote_by_connection_;
    };
} // namespace laps
