#include <quicr/quicr_common.h>
#include <quicr/quicr_server.h>

#include "client_subscriptions.h"
#include <set>

#include "cache.h"
#include "client_manager.h"

namespace laps {

ClientManager::ClientManager(const Config& cfg,
                              Cache& cache,
                              ClientSubscriptions& subscriptions,
                              peerQueue& peer_queue)
  : logger(std::make_shared<cantina::Logger>("CMGR", cfg.logger))
  , subscribeList(subscriptions)
  , config(cfg)
  , cache(cache)
  , client_mgr_id(cfg.client_config.listen_port)
  , _peer_queue(peer_queue)
{
}

quicr::PublishIntentResult ClientManager::onPublishIntent(const quicr::Namespace& quicr_namespace,
                                                          const std::string& /* origin_url */,
                                                          bool /* use_reliable_transport */,
                                                          const std::string& /* auth_token */,
                                                          quicr::bytes&& /* e2e_token */)
{

  FLOG_INFO("onPublishIntent namespace: " << quicr_namespace);

  _peer_queue.push(
    { .type = PeerObjectType::PUBLISH_INTENT, .source_peer_id = CLIENT_PEER_ID, .nspace = quicr_namespace });

  return { quicr::messages::Response::Ok, {}, {} };
}

void ClientManager::onPublishIntentEnd(
  const quicr::Namespace& quicr_namespace,
  [[maybe_unused]] const std::string& auth_token,
  [[maybe_unused]] quicr::bytes&& e2e_token) {

_peer_queue.push({ .type = PeerObjectType::PUBLISH_INTENT_DONE,
                    .source_peer_id = CLIENT_PEER_ID,
                    .nspace = quicr_namespace });
}

std::vector<quicr::PublishResult> ClientManager::onPublisherObject(
    const qtransport::TransportContextId &context_id,
    [[maybe_unused]] const qtransport::StreamId &stream_id,
    bool /* use_reliable_transport */,
    const quicr::messages::PublishDatagram &datagram) {

  if (not config.disable_dedup &&
      cache.exists(datagram.header.name, datagram.header.offset_and_fin)) {
    // duplicate, ignore
    FLOG_DEBUG("Duplicate message Name: " << datagram.header.name);
    return {};

  } else {
    cache.put(datagram.header.name, datagram.header.offset_and_fin,
              datagram.media_data);
  }

  // Send to peers
  _peer_queue.push({ .type =PeerObjectType::PUBLISH,.source_peer_id = CLIENT_PEER_ID, .pub_obj = datagram });

  auto list = subscribeList.find(datagram.header.name);

  std::vector<quicr::PublishResult> matching_subscriptions;
  for (const auto& cMgr : list) {
    for (const auto& dest : cMgr.second) {

      if (not config.disable_splithz &&
          dest.second.client_mgr_id == client_mgr_id &&
            dest.second.conn_id == context_id) {
            continue;
        }

        matching_subscriptions.push_back({
          .subscription_id = dest.second.subscribe_id,
          .priority = 1u,
          .expiry_ms = config.time_queue_ttl_default,
        });
    }
  }

  return matching_subscriptions;
}

quicr::SubscribeResult ClientManager::onSubscribe(
    const quicr::Namespace &quicr_namespace, const uint64_t& subscriber_id,
    const qtransport::TransportContextId &context_id,
    const qtransport::StreamId& stream_id,
    const quicr::SubscribeIntent /* subscribe_intent */,
    const std::string & /* origin_url */, bool /* use_reliable_transport */,
    const std::string & /* auth_token */, quicr::bytes && /* data */) {

  const auto& existing_remote =
    subscribeList.getSubscribeRemote(quicr_namespace, client_mgr_id, subscriber_id);

  if (existing_remote.client_mgr_id != 0) {
    FLOG_DEBUG("duplicate onSubscribe namespace: " << quicr_namespace << " subscriber_id: " << subscriber_id << " context_id"
                                              << context_id << " stream_id: " << stream_id);
    return { quicr::SubscribeResult::SubscribeStatus::FailedError, "", {}, {} };
  }

  FLOG_INFO("onSubscribe namespace: " << quicr_namespace << " subscriber_id: " << subscriber_id << " context_id "
                                      << context_id << " stream_id: " << stream_id);

  ClientSubscriptions::Remote remote = {
      .client_mgr_id = client_mgr_id,
      .subscribe_id = subscriber_id,
      .conn_id = context_id,
  };
  subscribeList.add(quicr_namespace.name(), quicr_namespace.length(),
                     client_mgr_id, remote);

  _peer_queue.push({ .type = PeerObjectType::SUBSCRIBE, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });

  return { quicr::SubscribeResult::SubscribeStatus::Ok, "", {}, {} };
}

void ClientManager::onUnsubscribe(const quicr::Namespace &quicr_namespace,
                                  const uint64_t &subscriber_id,
                                  const std::string & /* auth_token */) {

  FLOG_INFO("onUnsubscribe namespace: " << quicr_namespace << " subscriber_id: " << subscriber_id);

  subscribeList.remove(quicr_namespace.name(), quicr_namespace.length(),
                        client_mgr_id, subscriber_id);

  _peer_queue.push({ .type = PeerObjectType::UNSUBSCRIBE, .source_peer_id = CLIENT_PEER_ID,
                     .nspace = quicr_namespace });
}

} // namespace laps
