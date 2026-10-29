// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "admin_codec.h"

#include <quicr/utilities/bytes.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace laps {
    class ClientManager;
    class Config;

    /**
     * @brief Relay side of the admin RPC
     *
     * @details On start the relay records a subscribe-tracks interest for `base/relay-endpoint`.
     *      An admin client publishes requests on `base/relay-endpoint/client-endpoint` with track
     *      name `rpc`. Objects on that track are JSON requests. The relay publishes the JSON
     *      response on `base/client-endpoint/relay-endpoint`, again named `rpc`, which the client
     *      subscribes to directly.
     *
     *      The interest lives in the same subscribe-tracks map as a client SUBSCRIBE_TRACKS. It
     *      does not forward those publishes back out: the relay is the consumer.
     */
    class AdminService
    {
      public:
        AdminService(ClientManager& server, const Config& config);

        void Start();
        void Stop();

        /// True when this publish is an admin request track the relay itself wants.
        bool InterestedIn(const quicr::FullTrackName& track) const;

        /**
         * @brief Drop the reply track for an admin request that has ended
         *
         * @details The reply is a relay-local publish. It stays until the request publish goes away,
         *      which is the client session closing or a peered request being withdrawn.
         */
        void ReleaseRequest(const quicr::FullTrackName& request_track);

        /// Drop reply tracks whose admin client no longer has a request publish.
        void ReleaseRepliesExcept(const std::set<std::vector<std::string>>& live_client_keys);

        /**
         * @brief Warning when an rpc publish uses the admin base prefix but not this relay's endpoint
         *
         * @return Empty when the track is not an admin request aimed at a different relay endpoint
         */
        std::optional<std::string> ForeignRequestWarning(const quicr::FullTrackName& track) const;

        /**
         * @brief Handle one object when it is an admin request
         *
         * @return True when the object was an admin request and should not be forwarded as media
         */
        bool Consume(const quicr::FullTrackName& track, quicr::BytesSpan data);

        const std::vector<std::string>& SubscribePrefix() const noexcept { return subscribe_prefix_; }

      private:
        struct ReplyTrack
        {
            std::uint64_t track_fullname_hash{ 0 };
            std::uint64_t next_group_id{ 0 };
        };

        void ReleaseClient(const std::vector<std::string>& client_entries);
        ReplyTrack& EnsureReplyTrack(const std::vector<std::string>& client_entries);
        void PublishReply(const std::vector<std::string>& client_entries, const std::string& json);

        ClientManager& server_;
        const Config& config_;
        std::vector<std::string> base_entries_;
        std::vector<std::string> relay_entries_;
        std::vector<std::string> subscribe_prefix_;
        quicr::TrackNamespace local_prefix_;
        bool started_{ false };

        /// Keyed by the client namespace tuples. A slash inside one tuple must not collide with a suffix
        /// that is several tuples. A local request is consumed on the transport thread and a peered request
        /// on the peering thread, so the map is guarded.
        std::mutex reply_mutex_;
        std::map<std::vector<std::string>, ReplyTrack> reply_tracks_;
    };
} // namespace laps
