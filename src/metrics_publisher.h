// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "config.h"

#include <quicr/containers/safe_queue.h>
#include <quicr/metrics.h>
#include <quicr/session.h>
#include <quicr/track_name.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace laps {
    class ClientManager;

    /**
     * @brief Publishes relay metrics samples as JSON lines over MoQ tracks.
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
        void RemoveConnection(std::uint64_t connection_handle);

        void QueueConnectionMetrics(std::uint64_t connection_handle,
                                    std::size_t publish_track_count,
                                    const quicr::ConnectionMetrics& metrics);
        void QueuePublishMetrics(std::uint64_t connection_handle,
                                 const quicr::FullTrackName& track_name,
                                 const quicr::PublishTrackMetrics& metrics);
        void QueueSubscribeMetrics(std::uint64_t connection_handle,
                                   const quicr::FullTrackName& track_name,
                                   std::size_t subscriber_count,
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
            MetricType type;
            std::uint64_t connection_handle{ 0 };
            std::string json_line;
        };

        struct TrackState
        {
            quicr::FullTrackName full_track_name;
            std::uint64_t track_fullname_hash{ 0 };
            std::uint64_t next_object_id{ 0 };
        };

        struct RemoteInfo
        {
            std::string ip;
            std::uint16_t port{ 0 };
        };

        static constexpr std::uint32_t kQueueLimit = 5'000;

        void Run();
        void PublishSample(const MetricsSample& sample);
        void EnsureMetricsTracks();
        quicr::FullTrackName FullTrackNameFor(MetricType type) const;
        std::string MetricsNamespaceStr() const;
        static const char* TypeName(MetricType type);

        bool QueueSample(MetricsSample sample);

        ClientManager& server_;
        const Config& config_;
        std::vector<std::string> metrics_namespace_entries_;

        quicr::SafeQueue<MetricsSample> queue_{ kQueueLimit };
        std::atomic_bool running_{ false };
        std::thread worker_;

        std::mutex tracks_mutex_;
        std::map<MetricType, TrackState> tracks_;
        std::map<std::uint64_t, RemoteInfo> remote_by_connection_;
    };
} // namespace laps
