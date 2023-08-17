#include "client_subscriptions.h"

namespace laps {

ClientSubscriptions::ClientSubscriptions(const Config &cfg) : config(cfg) {
  subscriptions.resize(129 /* 128 + 1 to handle zero and 128 */);
}

void ClientSubscriptions::add(const quicr::Name& name, const int len,
                              uint16_t client_mgr_id,
                              const Remote& remote) {

  quicr::Namespace prefix(name, len);

  std::lock_guard<std::mutex> lock(mutex);

  auto mapIter = subscriptions[len].find(prefix.name());
  if (mapIter == subscriptions[len].end()) {
    // Add name and first subscriber for namespace
    subscriptions[len][prefix.name()][client_mgr_id].emplace(
        remote.subscribe_id, remote);

  } else if (mapIter->second.count(client_mgr_id) == 0) {
    // Add client mgr and subscriber to namespace
    mapIter->second[client_mgr_id].emplace(remote.subscribe_id, remote);

  } else if (mapIter->second[client_mgr_id].count(remote.subscribe_id) == 0) {
    // Add new subscriber to namespace/client_mgr_id
    mapIter->second[client_mgr_id].emplace(remote.subscribe_id, remote);
  }
}

void ClientSubscriptions::remove(const quicr::Name& name, const int len,
                                 uint16_t client_mgr_id,
                                 const Remote& remote) {

  remove(name, len, client_mgr_id, remote.subscribe_id);
}

void ClientSubscriptions::remove(const quicr::Name& name, const int len,
                                 uint16_t client_mgr_id,
                                 const uint64_t subscriber_id) {
  quicr::Namespace prefix(name, len);

  std::lock_guard<std::mutex> lock(mutex);

  auto mapIter = subscriptions[len].find(prefix.name());
  if (mapIter != subscriptions[len].end()) {

    auto cMgrIter = mapIter->second.find(client_mgr_id);
    if (cMgrIter == mapIter->second.end()) {
      // Not found, nothing to remove
      return;
    }

    if (cMgrIter->second.count(subscriber_id) > 0) {
      // Remove subscriber
      cMgrIter->second.erase(subscriber_id);

      if (cMgrIter->second.size() == 0) {
        // No subscribers, remove client mgr from namespace
        mapIter->second.erase(cMgrIter);

        if (mapIter->second.size() == 0) {
          // no client managers, remove namespace
          subscriptions[len].erase(mapIter);
        }
      }
    }
  }
}

const ClientSubscriptions::Remote ClientSubscriptions::getSubscribeRemote(const quicr::Namespace& ns,
                                                                          const uint16_t client_mgr_id,
                                                                          const uint64_t subscriber_id)
{
  const Remote empty_remote { .client_mgr_id = 0, .subscribe_id = 0, .conn_id = 0 };

  std::lock_guard<std::mutex> lock(mutex);

  if (subscriptions[ns.length()].empty()) {
    return empty_remote;
  }

  auto sublist_it = subscriptions[ns.length()].find(ns.name());
  if (sublist_it != subscriptions[ns.length()].end()) {

    const auto cmgr_it = sublist_it->second.find(client_mgr_id);
    if (cmgr_it == sublist_it->second.end()) {
      // Not found
      return empty_remote;
    }

    const auto sub_it = cmgr_it->second.find(subscriber_id);
    if (sub_it != cmgr_it->second.end()) {
      // Found subscriber
      return sub_it->second;
    }
  }

  return empty_remote;
}

std::map<uint16_t, std::map<uint64_t,ClientSubscriptions::Remote>>
ClientSubscriptions::find(const quicr::Name &name) {
  std::map<uint16_t, std::map<uint64_t, Remote>> ret;

  std::lock_guard<std::mutex> lock(mutex);

  // TODO: Fix this to not have to iterate for each mask bit
  for (int len = 0; len <= 128; len++) {
    quicr::Namespace prefix(name, len);

    auto mapIter = subscriptions[len].find(prefix.name());
    if (mapIter != subscriptions[len].end()) {

      for (const auto &cMgr: mapIter->second) {
        for (const auto &subs : cMgr.second) {
          // TODO: When actions are added, delete remote if filtered
          ret[subs.second.client_mgr_id].emplace(subs.first, subs.second);
        }
      }
    }
  }

  return ret;
}

} // namespace laps