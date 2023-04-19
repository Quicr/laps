#pragma once

#include <list>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <functional>

#include <quicr/encode.h>
#include <quicr/quicr_name.h>
#include <quicr/quicr_namespace.h>
#include <transport/transport.h>

#include "config.h"
#include "logger.h"

namespace laps {
class ClientSubscriptions {
public:
  struct Remote {
    const uint16_t client_mgr_id;
    const uint64_t subscribe_id;

    /*
     * TODO: Add action to indicate if remote should receive objects on this
     *     subscription or if it should be filtered out. This will replace
     *     split horizon.
     */
    const uint64_t conn_id;         // Only used for split horizon right now

    std::function<void(const quicr::messages::PublishDatagram& datagram)> sendObjFunc;

    bool operator==(const Remote &o) const {
      return client_mgr_id == o.client_mgr_id
             && subscribe_id == o.subscribe_id
             && conn_id == o.conn_id;
    }

    bool operator<(const Remote &o) const {
      return std::tie(client_mgr_id, subscribe_id, conn_id) <
             std::tie(o.client_mgr_id, o.subscribe_id, o.conn_id);
    }
  };

  ClientSubscriptions(const Config &cfg);

  void add(const quicr::Name& name, const int len,
           const uint16_t& client_mgr_id, const Remote& remote);

  void remove(const quicr::Name& name, const int len,
              const uint16_t& client_mgr_id, const Remote& remote);

  void remove(const quicr::Name& name, const int len,
              const uint16_t& client_mgr_id, const uint64_t& subscriber_id);

  std::map<uint16_t, std::map<uint64_t, Remote>> find(const quicr::Name &name);

private:
  // subscriptions[name length in bits][quicr name][client_mgr_id][subscriber id] = remote info
  std::vector<std::map<quicr::Name, std::map<uint16_t, std::map<uint64_t, Remote>>>> subscriptions;
  const Config& config;

  std::mutex mutex;
};

} // namespace laps
