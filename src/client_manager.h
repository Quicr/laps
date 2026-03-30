#pragma once

#include "config.h"
#include "peering/peer_manager.h"
#include "state.h"
#include "track_ranking.h"
#include "types.h"

#include <atomic>
#include <map>
#include <memory>

namespace laps {
    class ClientManager
    {
      public:
        ClientManager(State& state, const Config& config, peering::PeerManager& peer_manager);

        virtual bool Start() = 0;

      protected:
        State& state_;
        const Config& config_;
        peering::PeerManager& peer_manager_;

        /**
         * @brief Map of atomic bools to mark if a fetch thread should be interrupted.
         */
        std::map<std::pair<ConnectionHandle, RequestID>, std::atomic_bool> stop_fetch_;

        std::unordered_map<std::uint64_t, std::shared_ptr<TrackRanking>> track_rankings_;
    };

    std::shared_ptr<ClientManager> MakeClientManager(State& state,
                                                     const Config& config,
                                                     peering::PeerManager& peer_manager);
}
