#include "moxygen_client_manager.h"

#include "moq/shim/moxygen_subscribe_to_relay.h"
#include "publish_handler.h"
#include "subscribe_handler.h"

#include <moxygen/MoQSession.h>

#include <quicr/subscribe_track_handler.h>
#include <quicr/track_name.h>

#include <spdlog/spdlog.h>

#include <any>
#include <string>
#include <utility>
#include <vector>

namespace laps {

    /**
     * MoQSession keeps a raw pointer to the close callback; it does not take ownership.
     * We heap-allocate the hook and delete it from onMoQSessionClosed after unregistering.
     */
    class MoxygenClientManager::MoqSessionCloseHook final : public moxygen::MoQSession::MoQSessionCloseCallback
    {
      public:
        MoqSessionCloseHook(MoxygenClientManager& owner, ConnectionHandle handle)
          : owner_(owner)
          , handle_(handle)
        {
        }

        void onMoQSessionClosed() override
        {
            const auto qh = static_cast<quicr::ConnectionHandle>(handle_);
            owner_.moxygen_moq_port_->ClearUpstreamSubscriptionsFor(qh);
            owner_.moxygen_moq_port_->ClearInboundSubscriptionsFor(qh);
            owner_.moxygen_moq_port_->ClearRelayPublishBindingsForConnection(qh);
            owner_.moxygen_moq_port_->ClearOutboundFetchStateFor(qh);
            owner_.moxygen_moq_port_->ClearPeerFanoutPublishNamespaceHandlesForSubscriber(qh);
            owner_.moq_session_registry_.RemoveByHandle(handle_);
            delete this;
        }

      private:
        MoxygenClientManager& owner_;
        ConnectionHandle handle_;
    };

    MoxygenClientManager::MoxygenClientManager(State& state, const Config& config, peering::PeerManager& peer_manager)
      : ClientManager(state, config, peer_manager)
      , moxygen::MoQPicoQuicServer(
          config.tls_cert_filename_, config.tls_key_filename_, "/moxygen-relay", "16")
      , moxygen_moq_port_(std::make_shared<moq::shim::MoxygenMoqServerPort>(*this, config.logger_))
      , relay_core_(state, config, peer_manager, moxygen_moq_port_)
      , config_(config)
    {
    }

    MoxygenClientManager::~MoxygenClientManager()
    {
        stop();
    }

    bool MoxygenClientManager::Start()
    {
        folly::SocketAddress addr("::", config_.port);
        start(addr);

        return true;
    }

    moq::shim::AnnouncerSubscribeHandlerFactory MoxygenClientManager::GetAnnouncerSubscribeHandlerFactory()
    {
        return [this](const quicr::FullTrackName& full_track_name,
                      quicr::messages::ObjectPriority priority,
                      quicr::messages::GroupOrder group_order) {
            auto h = std::make_shared<SubscribeTrackHandler>(full_track_name,
                                                             static_cast<uint8_t>(priority),
                                                             group_order,
                                                             *this,
                                                             peer_manager_.GetTickService());
            h->SupportNewGroupRequest(true);
            return h;
        };
    }

    void MoxygenClientManager::ProcessSubscribe(quicr::ConnectionHandle connection_handle,
                                                uint64_t request_id,
                                                const quicr::TrackHash& th,
                                                const quicr::FullTrackName& track_full_name,
                                                const quicr::messages::SubscribeAttributes& attrs,
                                                std::optional<quicr::messages::Location> largest)
    {
        relay_core_.ProcessSubscribe(GetAnnouncerSubscribeHandlerFactory(),
                                     connection_handle,
                                     request_id,
                                     th,
                                     track_full_name,
                                     attrs,
                                     largest);
    }

    void MoxygenClientManager::RemoveOrPausePublisherSubscribe(quicr::TrackFullNameHash track_fullname_hash)
    {
        if (peer_manager_.HasSubscribers(track_fullname_hash)) {
            return;
        }

        bool has_subs = false;

        std::vector<std::pair<quicr::messages::TrackAlias, quicr::ConnectionHandle>> remove_sub_pub;
        for (auto it = state_.pub_subscribes.lower_bound({ track_fullname_hash, 0 }); it != state_.pub_subscribes.end();
             ++it) {
            const auto& key = it->first;
            const auto& sub_to_pub_handler = it->second;

            if (key.first != track_fullname_hash) {
                break;
            }

            if (sub_to_pub_handler->HasSubscribers()) {
                has_subs = true;
                continue;
            }

            if (sub_to_pub_handler->IsPublisherInitiated()) {
                SPDLOG_LOGGER_INFO(config_.logger_,
                                   "No subscribers left, pause publisher conn_id: {} track_alias: {}",
                                   key.second,
                                   track_fullname_hash);
                sub_to_pub_handler->Pause();
            } else {
                SPDLOG_LOGGER_INFO(config_.logger_,
                                   "No subscribers left, unsubscribe publisher conn_id: {} track_alias: {}",
                                   key.second,
                                   track_fullname_hash);

                moxygen_moq_port_->UnsubscribeTrack(
                  key.second,
                  std::static_pointer_cast<quicr::SubscribeTrackHandler>(sub_to_pub_handler));
                remove_sub_pub.push_back(key);
            }
        }

        for (const auto& key : remove_sub_pub) {
            state_.pub_subscribes.erase(key);
        }

        if (!has_subs) {
            peer_manager_.ClientUnsubscribe(track_fullname_hash);
        }
    }

    void MoxygenClientManager::ApplyPeerAnnouncePublishNamespace(
      quicr::ConnectionHandle connection_handle,
      const quicr::TrackNamespace& track_namespace,
      const quicr::PublishNamespaceAttributes& attrs)
    {
        auto [req_it, _] = state_.requests.try_emplace({ connection_handle, attrs.request_id },
                                                       State::RequestTransaction::Type::kPublishNamespace,
                                                       State::RequestTransaction::State::kOk);
        req_it->second.related_data.emplace<quicr::TrackNamespace>(track_namespace);

        auto subscribe_to_publisher = [&] {
            auto& anno_tracks = state_.pub_namespace_active[{ track_namespace, connection_handle }];

            for (const auto& [ns, sub_tracks] : state_.subscribe_active_) {
                if (!ns.first.HasSamePrefix(track_namespace)) {
                    continue;
                }

                if (sub_tracks.empty()) {
                    continue;
                }

                auto& a_si = *sub_tracks.begin();
                if (anno_tracks.find(a_si.track_alias) == anno_tracks.end()) {
                    SPDLOG_LOGGER_INFO(config_.logger_,
                                       "ApplyPeerAnnouncePublishNamespace (moxygen): subscribe to announcer ch={} "
                                       "track_alias={}",
                                       connection_handle,
                                       a_si.track_alias);

                    anno_tracks.insert(a_si.track_alias);

                    const auto& sub_info_it = state_.subscribes.find({ a_si.track_alias, a_si.connection_handle });
                    if (sub_info_it == state_.subscribes.end()) {
                        continue;
                    }

                    const auto& sub_ftn = sub_info_it->second.track_full_name;

                    auto sub_track_handler =
                      std::make_shared<SubscribeTrackHandler>(sub_ftn,
                                                              0,
                                                              quicr::messages::GroupOrder::kOriginalPublisherOrder,
                                                              *this,
                                                              peer_manager_.GetTickService());

                    for (const auto& sub_info : sub_tracks) {
                        sub_track_handler->AddSubscriber(sub_info.connection_handle,
                                                         sub_info.request_id,
                                                         sub_info.priority,
                                                         sub_info.delivery_timeout,
                                                         sub_info.start_location);
                    }

                    moxygen_moq_port_->SubscribeTrack(
                      connection_handle,
                      std::static_pointer_cast<quicr::SubscribeTrackHandler>(sub_track_handler));
                    state_.pub_subscribes[{ a_si.track_alias, connection_handle }] = sub_track_handler;
                }
            }
        };

        const auto th = quicr::TrackHash(quicr::FullTrackName{ track_namespace, {} });

        const auto existing_ns_it = state_.pub_namespace_active.find({ track_namespace, connection_handle });
        if (existing_ns_it != state_.pub_namespace_active.end()) {
            if (connection_handle) {
                subscribe_to_publisher();
            }
            SPDLOG_LOGGER_INFO(config_.logger_,
                               "ApplyPeerAnnouncePublishNamespace (moxygen): duplicate namespace ch={} ns_hash={}",
                               connection_handle,
                               th.track_namespace_hash);
            return;
        }

        state_.pub_namespace_active.try_emplace(std::make_pair(track_namespace, connection_handle));

        std::vector<quicr::ConnectionHandle> sub_annos_connections;
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }
            for (const auto& [conn_handle, _] : conns) {
                sub_annos_connections.emplace_back(conn_handle);
            }
        }

        moxygen_moq_port_->FanoutPublishNamespaceAnnounces(
          connection_handle, track_namespace, attrs.request_id, sub_annos_connections);

        if (connection_handle) {
            subscribe_to_publisher();
            peer_manager_.ClientAnnounce({ track_namespace, {} }, attrs, false);
        }

        SPDLOG_LOGGER_INFO(config_.logger_,
                           "ApplyPeerAnnouncePublishNamespace (moxygen): connection_handle={} namespace_hash={}",
                           connection_handle,
                           th.track_namespace_hash);
    }

    void MoxygenClientManager::ApplyPeerAnnouncePublishNamespaceDone(quicr::ConnectionHandle connection_handle,
                                                                     quicr::messages::RequestID request_id)
    {
        auto it = state_.requests.find({ connection_handle, request_id });
        if (it == state_.requests.end()) {
            SPDLOG_LOGGER_DEBUG(config_.logger_,
                                "ApplyPeerAnnouncePublishNamespaceDone (moxygen): no request state ch={} req_id={}",
                                connection_handle,
                                request_id);
            return;
        }

        auto& track_namespace = std::any_cast<quicr::TrackNamespace&>(it->second.related_data);

        std::vector<quicr::ConnectionHandle> sub_namespace_connections;
        for (const auto& [ns, conns] : state_.subscribes_namespaces) {
            if (!ns.HasSamePrefix(track_namespace)) {
                continue;
            }
            for (const auto& [conn_handle, _] : conns) {
                sub_namespace_connections.emplace_back(conn_handle);
            }
        }

        moxygen_moq_port_->FanoutPublishNamespaceDones(request_id, sub_namespace_connections);

        const auto anno_it = state_.pub_namespace_active.find({ track_namespace, connection_handle });
        if (anno_it != state_.pub_namespace_active.end()) {
            const auto track_aliases = anno_it->second;
            for (const auto track_alias : track_aliases) {
                const auto ps_it = state_.pub_subscribes.find({ track_alias, connection_handle });
                if (ps_it == state_.pub_subscribes.end()) {
                    continue;
                }
                const auto& ptd = ps_it->second;
                if (ptd != nullptr) {
                    moxygen_moq_port_->UnsubscribeTrack(
                      connection_handle,
                      std::static_pointer_cast<quicr::SubscribeTrackHandler>(ptd));
                }
                state_.pub_subscribes.erase(ps_it);
            }
        }

        state_.pub_namespace_active.erase({ track_namespace, connection_handle });

        if (connection_handle) {
            peer_manager_.ClientUnannounce({ track_namespace, {} });
        }

        state_.requests.erase(it);
    }

    void MoxygenClientManager::ApplyPeerAnnouncePublish(
      quicr::ConnectionHandle connection_handle,
      uint64_t request_id,
      const quicr::messages::PublishAttributes& publish_attributes,
      [[maybe_unused]] std::weak_ptr<quicr::SubscribeNamespaceHandler> sub_ns_handler)
    {
        auto th = quicr::TrackHash(publish_attributes.track_full_name);

        SPDLOG_LOGGER_INFO(config_.logger_,
                           "ApplyPeerAnnouncePublish (moxygen): connection_handle={} track_alias={} request_id={}",
                           connection_handle,
                           th.track_fullname_hash,
                           request_id);

        for (auto& [tn, conns] : state_.subscribes_namespaces) {
            if (tn.HasSamePrefix(publish_attributes.track_full_name.name_space)) {
                for (auto& [_, pub_ns_h] : conns) {
                    if (!GetConfig().allow_self && pub_ns_h->GetConnectionId() == connection_handle) {
                        continue;
                    }

                    const auto handler = PublishTrackHandler::Create(publish_attributes.track_full_name,
                                                                     quicr::TrackMode::kStream,
                                                                     publish_attributes.priority,
                                                                     publish_attributes.delivery_timeout.count(),
                                                                     publish_attributes.start_location,
                                                                     *this);
                    if (!handler->GetTrackAlias().has_value()) {
                        handler->SetTrackAlias(th.track_fullname_hash);
                    }

                    pub_ns_h->PublishTrack(handler);
                }
            }
        }

        auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(publish_attributes.track_full_name,
                                                                           0,
                                                                           quicr::messages::GroupOrder::kAscending,
                                                                           *this,
                                                                           peer_manager_.GetTickService(),
                                                                           true);

        if (publish_attributes.dynamic_groups) {
            sub_track_handler->SupportNewGroupRequest(true);
        }

        state_.pub_subscribes[{ th.track_fullname_hash, connection_handle }] = sub_track_handler;
        state_.pub_subscribes_by_req_id[{ request_id, connection_handle }] = sub_track_handler;

        if (connection_handle) {
            bool has_subs{ false };
            for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 }); it != state_.subscribes.end();
                 ++it) {
                if (it->first.first == th.track_fullname_hash) {
                    has_subs = true;
                    sub_track_handler->AddSubscriber(it->first.second,
                                                     it->second.request_id,
                                                     it->second.priority,
                                                     std::chrono::milliseconds(it->second.object_ttl),
                                                     it->second.start_location);
                }
            }

            quicr::messages::TrackNamespace sub_ns;
            for (const auto& [ns_prefix, c] : state_.subscribes_namespaces) {
                if (ns_prefix.HasSamePrefix(publish_attributes.track_full_name.name_space) && !c.empty()) {
                    has_subs = true;

                    if (sub_ns.empty()) {
                        sub_ns = ns_prefix;
                    }

                    for (const auto& [conn_id, ns_handler] : c) {
                        sub_track_handler->AddSubscribeNamespace(ns_handler);
                    }
                }
            }

            const auto ns_th = quicr::TrackHash({ sub_ns, {} });
            if (const auto rank_it = track_rankings_.find(ns_th.track_namespace_hash);
                rank_it != track_rankings_.end()) {
                sub_track_handler->SetTrackRanking(rank_it->second);
            }

            if (!has_subs) {
                SPDLOG_LOGGER_INFO(config_.logger_,
                                   "ApplyPeerAnnouncePublish (moxygen): no subscribers, pause ch={} track_alias={}",
                                   connection_handle,
                                   th.track_fullname_hash);
                sub_track_handler->Pause();
            }

            peer_manager_.ClientAnnounce(publish_attributes.track_full_name, {}, false);
        } else {
            sub_track_handler->SetTrackAlias(publish_attributes.track_alias);
        }
    }

    void MoxygenClientManager::RelayBindPublisherTrack(
      quicr::ConnectionHandle connection_handle,
      quicr::ConnectionHandle src_id,
      uint64_t request_id,
      const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
      bool ephemeral)
    {
        (void)src_id;
        (void)ephemeral;
        if (!track_handler) {
            return;
        }
        moxygen_moq_port_->RegisterRelayPublisherBinding(connection_handle, request_id, track_handler);
        SPDLOG_LOGGER_TRACE(config_.logger_,
                            "RelayBindPublisherTrack (moxygen): ch={} req_id={}",
                            connection_handle,
                            request_id);
    }

    void MoxygenClientManager::RelayUnbindPublisherTrack(
      quicr::ConnectionHandle connection_handle,
      quicr::ConnectionHandle src_id,
      const std::shared_ptr<quicr::PublishTrackHandler>& track_handler,
      bool send_publish_done)
    {
        (void)connection_handle;
        (void)src_id;
        (void)send_publish_done;
        if (track_handler) {
            moxygen_moq_port_->UnregisterRelayPublisherBinding(track_handler.get());
        }
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus> MoxygenClientManager::TryMoxygenRelayPublishObject(
      quicr::PublishTrackHandler* handler,
      quicr::TrackMode track_mode,
      const quicr::ObjectHeaders& object_headers,
      quicr::BytesSpan data)
    {
        return moxygen_moq_port_->TryRelayPublishObject(handler, track_mode, object_headers, data);
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
    MoxygenClientManager::TryMoxygenRelayForwardPublishedData(quicr::PublishTrackHandler* handler,
                                                              bool is_new_stream,
                                                              uint64_t group_id,
                                                              uint64_t subgroup_id,
                                                              std::shared_ptr<const std::vector<uint8_t>> data)
    {
        return moxygen_moq_port_->TryRelayForwardPublishedData(
          handler, is_new_stream, group_id, subgroup_id, std::move(data));
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus> MoxygenClientManager::TryMoxygenFetchPublishObject(
      quicr::PublishFetchHandler* handler,
      const quicr::ObjectHeaders& object_headers,
      quicr::BytesSpan data)
    {
        return moxygen_moq_port_->TryMoxygenFetchPublishObject(handler, object_headers, data);
    }

    std::optional<quicr::PublishTrackHandler::PublishObjectStatus>
    MoxygenClientManager::TryMoxygenFetchForwardPublishedData(quicr::PublishFetchHandler* handler,
                                                              bool is_new_stream,
                                                              uint64_t group_id,
                                                              uint64_t subgroup_id,
                                                              std::shared_ptr<const std::vector<uint8_t>> data)
    {
        return moxygen_moq_port_->TryMoxygenFetchForwardPublishedData(
          handler, is_new_stream, group_id, subgroup_id, std::move(data));
    }

    void MoxygenClientManager::onNewSession(std::shared_ptr<moxygen::MoQSession> client_session)
    {
        const ConnectionHandle session_handle = moq_session_registry_.RegisterSession(client_session);
        if (session_handle != 0) {
            client_session->setSessionCloseCallback(new MoqSessionCloseHook(*this, session_handle));
            SPDLOG_LOGGER_DEBUG(config_.logger_,
                                "MoQ session registered: connection_handle={}",
                                session_handle);
        }

        client_session->setPublishHandler(relay_);
        client_session->setSubscribeHandler(relay_);
    }

    std::shared_ptr<moxygen::MoQSession> MoxygenClientManager::createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor)
    {
        return std::make_shared<LapsMoqRelaySession>(
          folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)), *this, std::move(executor));
    }

    void MoxygenClientManager::OnInboundMoqSubscribeOk(
      const moxygen::SubscribeRequest& sub_req,
      std::optional<quicr::messages::Location> largest_from_subscribe_ok)
    {
        auto session = moxygen::MoQSession::getRequestSession();
        if (!session) {
            SPDLOG_LOGGER_WARN(config_.logger_, "OnInboundMoqSubscribeOk: missing MoQ request session");
            return;
        }

        ConnectionHandle handle = 0;
        if (!moq_session_registry_.TryFindHandle(session, handle) || handle == 0) {
            SPDLOG_LOGGER_WARN(config_.logger_,
                               "OnInboundMoqSubscribeOk: session not in registry (handle lookup failed)");
            return;
        }

        const quicr::ConnectionHandle quicr_handle = static_cast<quicr::ConnectionHandle>(handle);
        const quicr::messages::RequestID request_id = sub_req.requestID.value;
        if (state_.subscribe_alias_req_id.contains({ quicr_handle, request_id })) {
            SPDLOG_LOGGER_INFO(config_.logger_,
                               "OnInboundMoqSubscribeOk: duplicate subscribe (connection_handle={} request_id={}); "
                               "skipping RelayCore",
                               handle,
                               sub_req.requestID.value);
            return;
        }

        const quicr::FullTrackName track_full_name = moq::shim::MoxygenSubscribeToQuicrFullTrackName(sub_req.fullTrackName);
        const quicr::messages::SubscribeAttributes attrs = moq::shim::MoxygenSubscribeToQuicrAttributes(sub_req);
        const quicr::TrackHash th(track_full_name);

        relay_core_.ProcessSubscribe(GetAnnouncerSubscribeHandlerFactory(),
                                     quicr_handle,
                                     sub_req.requestID.value,
                                     th,
                                     track_full_name,
                                     attrs,
                                     largest_from_subscribe_ok);
    }

    void MoxygenClientManager::ProcessMoqIngressPublish(quicr::ConnectionHandle connection_handle,
                                                        const moxygen::PublishRequest& pub)
    {
        const auto publish_attributes = moq::shim::MoqShimMoxygenPublishRequestToQuicrPublishAttributes(pub);
        const uint64_t request_id = pub.requestID.value;
        const auto th = quicr::TrackHash(publish_attributes.track_full_name);

        SPDLOG_LOGGER_INFO(config_.logger_,
                           "ProcessMoqIngressPublish (moxygen): connection_handle={} track_alias={} request_id={}",
                           connection_handle,
                           th.track_fullname_hash,
                           request_id);

        for (auto& [tn, conns] : state_.subscribes_namespaces) {
            if (tn.HasSamePrefix(publish_attributes.track_full_name.name_space)) {
                for (auto& [_, pub_ns_h] : conns) {
                    if (!GetConfig().allow_self && pub_ns_h->GetConnectionId() == connection_handle) {
                        continue;
                    }

                    const auto p_handler = PublishTrackHandler::Create(publish_attributes.track_full_name,
                                                                       quicr::TrackMode::kStream,
                                                                       publish_attributes.priority,
                                                                       publish_attributes.delivery_timeout.count(),
                                                                       publish_attributes.start_location,
                                                                       *this);
                    if (!p_handler->GetTrackAlias().has_value()) {
                        p_handler->SetTrackAlias(th.track_fullname_hash);
                    }

                    pub_ns_h->PublishTrack(p_handler);
                }
            }
        }

        auto sub_track_handler = std::make_shared<SubscribeTrackHandler>(publish_attributes.track_full_name,
                                                                         0,
                                                                         quicr::messages::GroupOrder::kAscending,
                                                                         *this,
                                                                         peer_manager_.GetTickService(),
                                                                         true);

        if (publish_attributes.dynamic_groups) {
            sub_track_handler->SupportNewGroupRequest(true);
        }

        state_.pub_subscribes[{ th.track_fullname_hash, connection_handle }] = sub_track_handler;
        state_.pub_subscribes_by_req_id[{ request_id, connection_handle }] = sub_track_handler;

        if (connection_handle) {
            bool has_subs{ false };
            for (auto it = state_.subscribes.lower_bound({ th.track_fullname_hash, 0 }); it != state_.subscribes.end();
                 ++it) {
                if (it->first.first == th.track_fullname_hash) {
                    has_subs = true;
                    sub_track_handler->AddSubscriber(it->first.second,
                                                     it->second.request_id,
                                                     it->second.priority,
                                                     std::chrono::milliseconds(it->second.object_ttl),
                                                     it->second.start_location);
                }
            }

            quicr::messages::TrackNamespace sub_ns;
            for (const auto& [ns_prefix, c] : state_.subscribes_namespaces) {
                if (ns_prefix.HasSamePrefix(publish_attributes.track_full_name.name_space) && !c.empty()) {
                    has_subs = true;

                    if (sub_ns.empty()) {
                        sub_ns = ns_prefix;
                    }

                    for (const auto& [conn_id, ns_handler] : c) {
                        sub_track_handler->AddSubscribeNamespace(ns_handler);
                    }
                }
            }

            const auto ns_th = quicr::TrackHash({ sub_ns, {} });
            if (const auto rank_it = track_rankings_.find(ns_th.track_namespace_hash);
                rank_it != track_rankings_.end()) {
                sub_track_handler->SetTrackRanking(rank_it->second);
            }

            if (!has_subs) {
                SPDLOG_LOGGER_INFO(config_.logger_,
                                   "ProcessMoqIngressPublish (moxygen): no subscribers, pause ch={} track_alias={}",
                                   connection_handle,
                                   th.track_fullname_hash);
                sub_track_handler->Pause();
            }

            peer_manager_.ClientAnnounce(publish_attributes.track_full_name, {}, false);
        } else {
            sub_track_handler->SetTrackAlias(publish_attributes.track_alias);
        }
    }
} // namespace laps
