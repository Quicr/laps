// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "admin_service.h"

#include "client_manager.h"
#include "config.h"
#include "publish_namespace_handler.h"

#include <quicr/attributes.h>
#include <quicr/messages/object.h>

#include <limits>
#include <stdexcept>
#include <string_view>

namespace laps {
    namespace {
        constexpr std::size_t kMaxAdminPayloadBytes = 65'536;

        /// Subscribe-tracks interest owned by the relay. Matching publishes are consumed here.
        class LocalAdminNamespaceHandler : public PublishNamespaceHandler
        {
          public:
            using PublishNamespaceHandler::PublishNamespaceHandler;

            bool ForwardsPublishedTracks() const override { return false; }

            void PublishTrack(std::shared_ptr<quicr::PublishTrackHandler>) override {}
        };
    } // namespace

    AdminService::AdminService(ClientManager& server, const Config& config)
      : server_(server)
      , config_(config)
      , base_entries_(
          SplitTrackNamespace(config.admin_namespace_.empty() ? kDefaultAdminPrefix : config.admin_namespace_))
      , relay_entries_(SplitTrackNamespace(config.relay_id_))
    {
        subscribe_prefix_ = base_entries_;
        subscribe_prefix_.insert(subscribe_prefix_.end(), relay_entries_.begin(), relay_entries_.end());
    }

    void AdminService::Start()
    {
        if (base_entries_.empty() || relay_entries_.empty()) {
            throw std::invalid_argument("admin namespace and relay endpoint id must be non-empty");
        }
        if (subscribe_prefix_.size() >= 32) {
            throw std::invalid_argument("admin subscribe prefix exceeds 32 tuple entries");
        }

        local_prefix_ = quicr::TrackNamespace(subscribe_prefix_);
        auto handler = std::shared_ptr<LocalAdminNamespaceHandler>(
          new LocalAdminNamespaceHandler(local_prefix_, server_.GetTickService()));
        server_.InstallLocalAdminSubscribe(local_prefix_, handler);
        started_ = true;

        SPDLOG_LOGGER_INFO(config_.logger_,
                           "Admin subscribe tracks prefix: {} name {}",
                           JoinTrackNamespace(subscribe_prefix_),
                           kAdminRpcTrackName);
    }

    void AdminService::Stop()
    {
        if (!started_) {
            return;
        }
        started_ = false;
        server_.RemoveLocalAdminSubscribe(local_prefix_);
        reply_tracks_.clear();
        SPDLOG_LOGGER_INFO(config_.logger_, "Admin subscribe tracks stopped");
    }

    bool AdminService::InterestedIn(const quicr::FullTrackName& track) const
    {
        return started_ && track.NameStr() == kAdminRpcTrackName &&
               AdminClientSuffix(track.name_space, subscribe_prefix_).has_value();
    }

    std::optional<std::string> AdminService::ForeignRequestWarning(const quicr::FullTrackName& track) const
    {
        if (!started_ || InterestedIn(track) || track.NameStr() != kAdminRpcTrackName) {
            return std::nullopt;
        }

        const auto& entries = track.name_space.GetEntries();
        if (entries.size() < base_entries_.size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < base_entries_.size(); ++i) {
            const std::string_view entry(reinterpret_cast<const char*>(entries[i].data()), entries[i].size());
            if (entry != base_entries_[i]) {
                return std::nullopt;
            }
        }

        return "Admin request " + track.NamespaceStr() + " (" + std::string(track.NameStr()) +
               ") is outside subscribe prefix " + JoinTrackNamespace(subscribe_prefix_) +
               "; --relay-endpoint must be this relay's --endpoint_id";
    }

    bool AdminService::Consume(const quicr::FullTrackName& track, quicr::BytesSpan data)
    {
        if (!InterestedIn(track)) {
            return false;
        }

        const auto client_entries = AdminClientSuffix(track.name_space, subscribe_prefix_);
        if (!client_entries.has_value()) {
            return false;
        }
        const auto client_key = JoinTrackNamespace(*client_entries);

        try {
            std::string response;
            if (data.size() > kMaxAdminPayloadBytes) {
                response = EncodeAdminError("", "request exceeds 65536 bytes");
            } else {
                const std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
                const auto message = ParseAdminMessage(payload);
                if (!message.valid || !message.has_method || message.method.empty()) {
                    const auto reason = message.error.empty() ? "request requires a method" : message.error;
                    response = EncodeAdminError(message.id, reason);
                } else if (message.method == kAdminStateSizesMethod) {
                    SPDLOG_LOGGER_INFO(
                      config_.logger_, "Admin RPC {} from {} id {}", message.method, client_key, message.id);
                    response = EncodeAdminResult(message.id, EncodeAdminStateSizes(server_.CollectAdminStateSizes()));
                } else {
                    SPDLOG_LOGGER_INFO(config_.logger_,
                                       "Admin RPC unknown method {} from {} id {}",
                                       message.method,
                                       client_key,
                                       message.id);
                    response = EncodeAdminError(message.id, "unknown method");
                }
            }

            PublishReply(client_key, *client_entries, response);
        } catch (const std::exception& ex) {
            SPDLOG_LOGGER_WARN(config_.logger_, "Admin RPC failed for {}: {}", client_key, ex.what());
            try {
                PublishReply(client_key, *client_entries, EncodeAdminError("", ex.what()));
            } catch (const std::exception& publish_error) {
                SPDLOG_LOGGER_WARN(config_.logger_, "Admin RPC error response failed: {}", publish_error.what());
            }
        }

        return true;
    }

    AdminService::ReplyTrack& AdminService::EnsureReplyTrack(const std::string& client_key,
                                                             const std::vector<std::string>& client_entries)
    {
        if (const auto it = reply_tracks_.find(client_key); it != reply_tracks_.end()) {
            return it->second;
        }

        std::vector<std::string> entries = base_entries_;
        entries.insert(entries.end(), client_entries.begin(), client_entries.end());
        entries.insert(entries.end(), relay_entries_.begin(), relay_entries_.end());

        const auto full_track_name = AdminFullTrackName(entries);
        const auto track_hash = quicr::TrackHash(full_track_name);
        const quicr::PublishAttributes attributes{ full_track_name,
                                                   track_hash.track_fullname_hash,
                                                   {},
                                                   std::nullopt,
                                                   std::nullopt,
                                                   true,
                                                   quicr::messages::GroupOrder::kAscending,
                                                   false,
                                                   kDefaultPriority,
                                                   std::nullopt,
                                                   config_.object_ttl_,
                                                   {} };
        server_.RegisterLocalPublish(attributes);

        SPDLOG_LOGGER_INFO(config_.logger_,
                           "Admin reply track namespace: {} name {}",
                           full_track_name.NamespaceStr(),
                           full_track_name.NameStr());

        const auto [it, _] = reply_tracks_.emplace(client_key, ReplyTrack{ track_hash.track_fullname_hash, 0 });
        return it->second;
    }

    void AdminService::PublishReply(const std::string& client_key,
                                    const std::vector<std::string>& client_entries,
                                    const std::string& json)
    {
        auto& track = EnsureReplyTrack(client_key, client_entries);
        std::vector<std::uint8_t> payload(json.begin(), json.end());

        const auto ttl = config_.object_ttl_ > std::numeric_limits<std::uint16_t>::max()
                           ? std::numeric_limits<std::uint16_t>::max()
                           : static_cast<std::uint16_t>(config_.object_ttl_);
        const quicr::ObjectHeaders headers{ .group_id = track.next_group_id++,
                                            .object_id = 0,
                                            .subgroup_id = 0,
                                            .payload_length = payload.size(),
                                            .status = quicr::ObjectStatus::kAvailable,
                                            .priority = kDefaultPriority,
                                            .ttl = ttl,
                                            .track_mode = quicr::TrackMode::kStream,
                                            .extensions = std::nullopt,
                                            .immutable_extensions = std::nullopt };

        if (!server_.PublishLocalObject(track.track_fullname_hash, headers, payload)) {
            SPDLOG_LOGGER_WARN(config_.logger_, "Admin reply track is not registered for {}", client_key);
        }
    }
} // namespace laps
