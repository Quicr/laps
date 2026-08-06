// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "metrics_publisher.h"

#include "client_manager.h"

#include <quicr/attributes.h>
#include <quicr/messages/object.h>

#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace laps {
    namespace {
        constexpr std::string_view kMetricsSchemaTrackName = "laps.metrics.openapi.v1";

        std::vector<std::string> SplitNamespace(const std::string& value, const std::string& relay_id)
        {
            const std::string namespace_value = value.empty() ? "metrics/" + relay_id : value;
            std::vector<std::string> entries;

            std::size_t start = 0;
            while (start <= namespace_value.size()) {
                const auto slash = namespace_value.find('/', start);
                const auto end = slash == std::string::npos ? namespace_value.size() : slash;
                if (end > start) {
                    entries.emplace_back(namespace_value.substr(start, end - start));
                }
                if (slash == std::string::npos) {
                    break;
                }
                start = slash + 1;
            }

            if (entries.empty()) {
                entries.emplace_back("metrics");
                entries.emplace_back(relay_id);
            }

            return entries;
        }

        std::vector<std::uint8_t> BytesFromString(std::string_view value)
        {
            return { value.begin(), value.end() };
        }

        std::string JsonEscape(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());

            constexpr char kHex[] = "0123456789abcdef";
            for (const auto ch : value) {
                const auto c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':
                        escaped += "\\\"";
                        break;
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '\b':
                        escaped += "\\b";
                        break;
                    case '\f':
                        escaped += "\\f";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        if (c < 0x20 || c > 0x7e) {
                            escaped += "\\u00";
                            escaped.push_back(kHex[(c >> 4) & 0x0f]);
                            escaped.push_back(kHex[c & 0x0f]);
                        } else {
                            escaped.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }

            return escaped;
        }

        void AppendComma(std::ostringstream& out, bool& first)
        {
            if (!first) {
                out << ',';
            }
            first = false;
        }

        void AppendStringField(std::ostringstream& out, bool& first, std::string_view key, std::string_view value)
        {
            AppendComma(out, first);
            out << '"' << key << "\":\"" << JsonEscape(value) << '"';
        }

        void AppendUintField(std::ostringstream& out, bool& first, std::string_view key, std::uint64_t value)
        {
            AppendComma(out, first);
            out << '"' << key << "\":" << value;
        }

        void AppendRawField(std::ostringstream& out, bool& first, std::string_view key, std::string_view value)
        {
            AppendComma(out, first);
            out << '"' << key << "\":" << value;
        }

        std::string SerializeMinMaxAvg(const quicr::MinMaxAvg& metrics)
        {
            std::ostringstream out;
            bool first = true;
            out << '{';
            AppendUintField(out, first, "min", metrics.min);
            AppendUintField(out, first, "max", metrics.max);
            AppendUintField(out, first, "avg", metrics.avg);
            AppendUintField(out, first, "value_sum", metrics.value_sum);
            AppendUintField(out, first, "value_count", metrics.value_count);
            out << '}';
            return out.str();
        }

        std::string SerializeConnectionQuic(const quicr::QuicConnectionMetrics& metrics)
        {
            std::ostringstream out;
            bool first = true;
            out << '{';
            AppendUintField(out, first, "cwin_congested", metrics.cwin_congested);
            AppendUintField(out, first, "prev_cwin_congested", metrics.prev_cwin_congested);
            AppendUintField(out, first, "tx_congested", metrics.tx_congested);
            AppendRawField(out, first, "tx_rate_bps", SerializeMinMaxAvg(metrics.tx_rate_bps));
            AppendRawField(out, first, "rx_rate_bps", SerializeMinMaxAvg(metrics.rx_rate_bps));
            AppendRawField(out, first, "tx_cwin_bytes", SerializeMinMaxAvg(metrics.tx_cwin_bytes));
            AppendRawField(out, first, "tx_in_transit_bytes", SerializeMinMaxAvg(metrics.tx_in_transit_bytes));
            AppendRawField(out, first, "rtt_us", SerializeMinMaxAvg(metrics.rtt_us));
            AppendRawField(out, first, "srtt_us", SerializeMinMaxAvg(metrics.srtt_us));
            AppendUintField(out, first, "tx_retransmits", metrics.tx_retransmits);
            AppendUintField(out, first, "tx_lost_pkts", metrics.tx_lost_pkts);
            AppendUintField(out, first, "tx_timer_losses", metrics.tx_timer_losses);
            AppendUintField(out, first, "tx_spurious_losses", metrics.tx_spurious_losses);
            AppendUintField(out, first, "rx_dgrams", metrics.rx_dgrams);
            AppendUintField(out, first, "rx_dgrams_bytes", metrics.rx_dgrams_bytes);
            AppendUintField(out, first, "tx_dgram_cb", metrics.tx_dgram_cb);
            AppendUintField(out, first, "tx_dgram_ack", metrics.tx_dgram_ack);
            AppendUintField(out, first, "tx_dgram_lost", metrics.tx_dgram_lost);
            AppendUintField(out, first, "tx_dgram_spurious", metrics.tx_dgram_spurious);
            AppendUintField(out, first, "tx_dgram_drops", metrics.tx_dgram_drops);
            out << '}';
            return out.str();
        }

        std::string SerializePublishQuic(const quicr::PublishTrackMetrics::Quic& metrics)
        {
            std::ostringstream out;
            bool first = true;
            out << '{';
            AppendUintField(out, first, "tx_buffer_drops", metrics.tx_buffer_drops);
            AppendUintField(out, first, "tx_queue_discards", metrics.tx_queue_discards);
            AppendUintField(out, first, "tx_queue_expired", metrics.tx_queue_expired);
            AppendUintField(out, first, "tx_delayed_callback", metrics.tx_delayed_callback);
            AppendUintField(out, first, "tx_reset_wait", metrics.tx_reset_wait);
            AppendRawField(out, first, "tx_queue_size", SerializeMinMaxAvg(metrics.tx_queue_size));
            AppendRawField(out, first, "tx_callback_ms", SerializeMinMaxAvg(metrics.tx_callback_ms));
            AppendRawField(out, first, "tx_object_duration_us", SerializeMinMaxAvg(metrics.tx_object_duration_us));
            out << '}';
            return out.str();
        }

        void AppendCommonFields(std::ostringstream& out,
                                bool& first,
                                std::string_view type,
                                const std::string& relay_id,
                                std::uint64_t sample_time_us,
                                std::uint64_t connection_handle)
        {
            AppendStringField(out, first, "type", type);
            AppendStringField(out, first, "relay_id", relay_id);
            AppendUintField(out, first, "sample_time_us", sample_time_us);
            AppendUintField(out, first, "connection_handle", connection_handle);
        }
    } // namespace

    MetricsPublisher::MetricsPublisher(ClientManager& server, const Config& config)
      : server_(server)
      , config_(config)
      , metrics_namespace_entries_(SplitNamespace(config.metrics_namespace_, config.relay_id_))
    {
    }

    MetricsPublisher::~MetricsPublisher()
    {
        Stop();
    }

    void MetricsPublisher::Start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }

        EnsureMetricsTracks();
        worker_ = std::thread(&MetricsPublisher::WorkerRun, this);
        SPDLOG_LOGGER_INFO(config_.logger_,
                           "Metrics publishing enabled on namespace {} name {} (batch interval {}ms)",
                           MetricsNamespaceStr(),
                           kMetricsSchemaTrackName,
                           kBatchInterval.count());
    }

    void MetricsPublisher::Stop()
    {
        const bool was_running = running_.exchange(false);
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_cv_.notify_all();
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        queue_.Clear();
        {
            std::lock_guard<std::mutex> lock(pending_batch_mutex_);
            pending_batch_.reset();
        }

        if (was_running) {
            SPDLOG_LOGGER_INFO(config_.logger_, "Metrics publishing stopped");
        }
    }

    void MetricsPublisher::AddConnection(std::uint64_t connection_handle,
                                         const quicr::Session::ConnectionRemoteInfo& remote)
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto& remote_info = remote_by_connection_[connection_handle];
        remote_info.ip = remote.ip;
        remote_info.port = remote.port;
    }

    void MetricsPublisher::SetConnectionEndpointId(std::uint64_t connection_handle, const std::string& endpoint_id)
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        remote_by_connection_[connection_handle].endpoint_id = endpoint_id;
    }

    void MetricsPublisher::RemoveConnection(std::uint64_t connection_handle)
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        remote_by_connection_.erase(connection_handle);
    }

    MetricsPublisher::RemoteInfo MetricsPublisher::LookupRemote(std::uint64_t connection_handle)
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (const auto it = remote_by_connection_.find(connection_handle); it != remote_by_connection_.end()) {
            return it->second;
        }

        return {};
    }

    void MetricsPublisher::QueueConnectionMetrics(std::uint64_t connection_handle,
                                                  std::size_t publish_track_count,
                                                  const quicr::ConnectionMetrics& metrics)
    {
        const auto remote_info = LookupRemote(connection_handle);

        std::ostringstream out;
        bool first = true;
        out << '{';
        AppendCommonFields(out,
                           first,
                           TypeName(MetricType::kConnection),
                           config_.relay_id_,
                           metrics.last_sample_time,
                           connection_handle);
        AppendStringField(out, first, "remote_endpoint_id", remote_info.endpoint_id);
        AppendStringField(out, first, "remote_ip", remote_info.ip);
        AppendUintField(out, first, "remote_port", remote_info.port);
        AppendUintField(out, first, "publish_tracks", static_cast<std::uint64_t>(publish_track_count));
        AppendUintField(out, first, "rx_dgram_unknown_track_alias", metrics.rx_dgram_unknown_track_alias);
        AppendUintField(out, first, "rx_dgram_invalid_type", metrics.rx_dgram_invalid_type);
        AppendUintField(out, first, "rx_dgram_decode_failed", metrics.rx_dgram_decode_failed);
        AppendUintField(out, first, "rx_stream_buffer_error", metrics.rx_stream_buffer_error);
        AppendUintField(out, first, "rx_stream_unknown_track_alias", metrics.rx_stream_unknown_track_alias);
        AppendUintField(out, first, "rx_stream_invalid_type", metrics.rx_stream_invalid_type);
        AppendUintField(out, first, "invalid_ctrl_stream_msg", metrics.invalid_ctrl_stream_msg);
        AppendRawField(out, first, "quic", SerializeConnectionQuic(metrics.quic));
        out << "}\n";

        QueueSample({ out.str() });
    }

    void MetricsPublisher::QueuePublishMetrics(std::uint64_t connection_handle,
                                               const quicr::FullTrackName& track_name,
                                               std::size_t subscriber_count,
                                               const quicr::PublishTrackMetrics& metrics)
    {
        const auto track_namespace = track_name.NamespaceStr();
        const auto name = track_name.NameStr();
        const auto remote_info = LookupRemote(connection_handle);

        std::ostringstream out;
        bool first = true;
        out << '{';
        AppendCommonFields(
          out, first, TypeName(MetricType::kPublish), config_.relay_id_, metrics.last_sample_time, connection_handle);
        AppendStringField(out, first, "remote_endpoint_id", remote_info.endpoint_id);
        AppendStringField(out, first, "track_namespace", track_namespace);
        AppendStringField(out, first, "track_name", name);
        AppendUintField(out, first, "bytes_published", metrics.bytes_published);
        AppendUintField(out, first, "objects_published", metrics.objects_published);
        AppendUintField(out, first, "objects_dropped_not_ok", metrics.objects_dropped_not_ok);
        AppendUintField(out, first, "subscribers", static_cast<std::uint64_t>(subscriber_count));
        AppendRawField(out, first, "quic", SerializePublishQuic(metrics.quic));
        out << "}\n";

        QueueSample({ out.str() });
    }

    void MetricsPublisher::QueueSubscribeMetrics(std::uint64_t connection_handle,
                                                 const quicr::FullTrackName& track_name,
                                                 const quicr::SubscribeTrackMetrics& metrics)
    {
        const auto track_namespace = track_name.NamespaceStr();
        const auto name = track_name.NameStr();

        std::ostringstream out;
        bool first = true;
        out << '{';
        AppendCommonFields(
          out, first, TypeName(MetricType::kSubscribe), config_.relay_id_, metrics.last_sample_time, connection_handle);
        AppendStringField(out, first, "track_namespace", track_namespace);
        AppendStringField(out, first, "track_name", name);
        AppendUintField(out, first, "bytes_received", metrics.bytes_received);
        AppendUintField(out, first, "objects_received", metrics.objects_received);
        out << "}\n";

        QueueSample({ out.str() });
    }

    bool MetricsPublisher::QueueSample(MetricsSample sample)
    {
        if (!running_) {
            return false;
        }

        const auto pushed = queue_.Push(std::move(sample));
        if (!pushed) {
            SPDLOG_LOGGER_DEBUG(config_.logger_, "Metrics queue full, dropped oldest sample");
        }

        /*
         * Queue*Metrics() (and therefore this) is only ever called from MetricsSampled() callbacks, which
         * the relay's transport dispatches on its single control/data thread. Publishing a batch touches
         * ClientManager state and libquicr handler internals that are only safe to touch from that same
         * thread, so the actual publish must happen here rather than on worker_. This call is cheap: almost
         * always there's no batch ready yet and it's just a mutex check.
         */
        FlushPendingBatch();

        return pushed;
    }

    void MetricsPublisher::FlushPendingBatch()
    {
        std::optional<MetricsSample> batch;
        {
            std::lock_guard<std::mutex> lock(pending_batch_mutex_);
            batch.swap(pending_batch_);
        }

        if (batch.has_value()) {
            PublishSample(*batch);
        }
    }

    void MetricsPublisher::WorkerRun()
    {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        while (running_) {
            worker_cv_.wait_for(lock, kBatchInterval);
            if (!running_) {
                break;
            }

            lock.unlock();

            // Coalesce whatever has queued up since the last batch into a single JSON-lines payload. This is
            // pure CPU work: it never touches ClientManager or libquicr handler state, so it's safe here.
            std::string batched;
            std::size_t count = 0;
            while (auto sample = queue_.Pop()) {
                batched += sample->json_line;
                ++count;
            }

            if (count > 0) {
                std::lock_guard<std::mutex> pending_lock(pending_batch_mutex_);
                if (pending_batch_.has_value()) {
                    pending_batch_->json_line += batched;
                } else {
                    pending_batch_ = MetricsSample{ std::move(batched) };
                }

                SPDLOG_LOGGER_DEBUG(config_.logger_, "Metrics batch ready: {} samples coalesced", count);
            }

            lock.lock();
        }
    }

    void MetricsPublisher::PublishSample(const MetricsSample& sample)
    {
        struct PublishTarget
        {
            std::uint64_t track_fullname_hash{ 0 };
            std::uint64_t group_id{ 0 };
        };

        std::optional<PublishTarget> target;
        {
            std::lock_guard<std::mutex> lock(tracks_mutex_);
            if (metrics_track_.has_value()) {
                target = PublishTarget{ metrics_track_->track_fullname_hash, metrics_track_->next_group_id++ };
            }
        }

        if (!target.has_value()) {
            EnsureMetricsTracks();
            std::lock_guard<std::mutex> lock(tracks_mutex_);
            if (!metrics_track_.has_value()) {
                return;
            }

            target = PublishTarget{ metrics_track_->track_fullname_hash, metrics_track_->next_group_id++ };
        }

        if (target->track_fullname_hash == 0) {
            return;
        }

        std::vector<std::uint8_t> payload(sample.json_line.begin(), sample.json_line.end());

        quicr::ObjectHeaders headers{};
        headers.group_id = target->group_id;
        headers.object_id = 0;
        headers.subgroup_id = 0;
        headers.payload_length = payload.size();
        headers.status = quicr::ObjectStatus::kAvailable;
        headers.priority = kDefaultPriority;
        headers.ttl = static_cast<std::uint16_t>(kDefaultObjectTtl);
        headers.track_mode = quicr::TrackMode::kStream;

        try {
            if (!server_.PublishLocalObject(target->track_fullname_hash, headers, payload)) {
                SPDLOG_LOGGER_DEBUG(config_.logger_,
                                    "Metrics local publish track missing for track alias {}",
                                    target->track_fullname_hash);
            }
        } catch (const std::exception& e) {
            SPDLOG_LOGGER_DEBUG(config_.logger_, "Metrics publish failed: {}", e.what());
        }
    }

    void MetricsPublisher::EnsureMetricsTracks()
    {
        {
            std::lock_guard<std::mutex> lock(tracks_mutex_);
            if (metrics_track_.has_value()) {
                return;
            }
        }

        auto full_track_name = MetricsTrackName();
        const auto th = quicr::TrackHash(full_track_name);

        SPDLOG_LOGGER_DEBUG(config_.logger_,
                            "Creating relay-local metrics publish track namespace: {} name: {} alias: {}",
                            full_track_name.NamespaceStr(),
                            full_track_name.NameStr(),
                            th.track_fullname_hash);

        const quicr::PublishAttributes attrs{ full_track_name,
                                              th.track_fullname_hash,
                                              {},
                                              std::nullopt,
                                              std::nullopt,
                                              true,
                                              quicr::messages::GroupOrder::kAscending,
                                              false,
                                              kDefaultPriority,
                                              std::nullopt,
                                              kDefaultObjectTtl,
                                              {} };
        server_.RegisterLocalPublish(attrs);

        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (!metrics_track_.has_value()) {
            metrics_track_ = TrackState{ std::move(full_track_name), th.track_fullname_hash, 0 };
        }
    }

    quicr::FullTrackName MetricsPublisher::MetricsTrackName() const
    {
        quicr::FullTrackName full_track_name;
        full_track_name.name_space = quicr::TrackNamespace(metrics_namespace_entries_);
        full_track_name.name = BytesFromString(kMetricsSchemaTrackName);
        return full_track_name;
    }

    std::string MetricsPublisher::MetricsNamespaceStr() const
    {
        return quicr::TrackNamespace(metrics_namespace_entries_).Str();
    }

    const char* MetricsPublisher::TypeName(MetricType type)
    {
        switch (type) {
            case MetricType::kConnection:
                return "connection";
            case MetricType::kSubscribe:
                return "subscribe";
            case MetricType::kPublish:
                return "publish";
        }

        return "unknown";
    }
} // namespace laps
