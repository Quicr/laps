#pragma once

#include <cstdint>

/**
 * @file moxygen_native_quic_target.h
 * @brief P0/P1: Native QUIC MoQ listener choice for laps (see design doc §11).
 *
 * **P1 (done in laps):** `MoxygenClientManager` uses `moxygen::MoQPicoQuicServer` — picoquic UDP
 * listener, MoQT ALPNs from `getMoqtProtocols`, **no** mvfst/HQ `MoQServer`. Sessions still use
 * `MoQSession` over `PicoQuicWebTransport` (proxygen’s WebTransport API as a thin adapter over
 * picoquic streams), which is **not** HTTP/3 upgrade.
 *
 * **Optional later:** `MoQPicoQuicEventBaseServer` if laps wants picoquic I/O on a shared
 * `folly::EventBase` instead of picoquic’s dedicated network thread.
 */

namespace laps::moq::shim {

    /**
     * @brief Moxygen server transport variant for native QUIC MoQ (documentation / future config).
     */
    enum class MoxygenNativeMoqEntrypoint : std::uint8_t
    {
        kPicoQuicEventBaseServer = 0,
        /** Used by `MoxygenClientManager` today (P1 complete). */
        kPicoQuicServer = 1,
    };

    constexpr MoxygenNativeMoqEntrypoint kDefaultNativeMoqEntrypoint = MoxygenNativeMoqEntrypoint::kPicoQuicServer;

} // namespace laps::moq::shim
