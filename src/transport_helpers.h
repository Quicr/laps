// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <quicr/connection.h>
#include <quicr/transport.h>

#include <arpa/inet.h>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace laps {

    /**
     * @brief Peer IP address and port of a connection
     *
     * @details libquicr no longer reports the remote endpoint with the new connection callback, so it has to
     *      be queried from the transport that owns the connection.
     *
     * @param transport     Transport that owns the connection
     * @param connection    Connection to get the peer address of
     *
     * @returns Pair of IP address string and port. Empty address and zero port if unavailable.
     */
    inline std::pair<std::string, std::uint16_t> GetPeerAddress(const std::shared_ptr<quicr::Transport>& transport,
                                                                const std::shared_ptr<quicr::Connection>& connection)
    {
        if (transport == nullptr || connection == nullptr) {
            return {};
        }

        sockaddr_storage addr{};
        if (!transport->GetPeerAddrInfo(connection, &addr)) {
            return {};
        }

        char ip[INET6_ADDRSTRLEN]{ 0 };

        switch (addr.ss_family) {
            case AF_INET: {
                const auto* sa = reinterpret_cast<const sockaddr_in*>(&addr);
                if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) == nullptr) {
                    return {};
                }
                return { ip, ntohs(sa->sin_port) };
            }
            case AF_INET6: {
                const auto* sa = reinterpret_cast<const sockaddr_in6*>(&addr);
                if (inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof(ip)) == nullptr) {
                    return {};
                }
                return { ip, ntohs(sa->sin6_port) };
            }
            default:
                return {};
        }
    }

} // namespace laps
