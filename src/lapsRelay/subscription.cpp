#include "subscription.h"

namespace laps {

Subscriptions::Subscriptions(const Config &cfg) : config(cfg) {
  subscriptions.resize(129 /* 128 + 1 to handle zero and 128 */);
}

void Subscriptions::add(const quicr::Name &name, const int len,
                        const Remote &remote) {

  quicr::Namespace prefix(name, len);

  auto mapPtr = subscriptions[len].find(prefix.name());
  if (mapPtr == subscriptions[len].end()) {
    // Add name and first subscriber
    subscriptions[len][prefix.name()].emplace(remote.subscribe_id, remote);

  } else {
    if (mapPtr->second.count(remote.subscribe_id) > 0) {
      // Add new subscriber to name/len map
      mapPtr->second.emplace(remote.subscribe_id, remote);
    }
  }
}

void Subscriptions::remove(const quicr::Name &name, const int len,
                           const Remote &remote) {

  remove(name, len, remote.subscribe_id);
}

void Subscriptions::remove(const quicr::Name &name, const int len,
                             const uint64_t& subscriber_id) {
    quicr::Namespace prefix(name, len);

    auto mapPtr = subscriptions[len].find(prefix.name());
    if (mapPtr != subscriptions[len].end()) {
      if (mapPtr->second.count(subscriber_id) > 0) {
        // Remove subscriber
        mapPtr->second.erase(subscriber_id);

        if (mapPtr->second.size() == 0) {
          // No subscribers, remove name from map
          subscriptions[len].erase(mapPtr);
        }
      }
    }
  }

std::list<Subscriptions::Remote> Subscriptions::find(const quicr::Name &name) {
  std::list<Remote> ret;

  // TODO: Fix this to not have to iterate for each mask bit
  for (int len = 0; len <= 128; len++) {
    quicr::Namespace prefix(name, len);

    auto mapPtr = subscriptions[len].find(prefix.name());
    if (mapPtr != subscriptions[len].end()) {

      for (const auto& subs : mapPtr->second ) {
        ret.push_back(subs.second);
      }
    }
  }

  return ret;
}

} // namespace laps