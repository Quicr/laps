#pragma once

/**
 * @file moxygen_quicr_object_mapping.h
 * @brief P0: Notes for object / media path (SubscribeTrackHandler <-> TrackConsumer).
 *
 * - quicr::ObjectHeaders + quicr::Bytes map to moxygen object metadata + payload buffers
 *   (often folly::IOBuf chains). Exact field mapping depends on MoQ object header layout
 *   in both stacks; implement in handler bridge .cc with unit tests.
 * - Group ID / object ID / publisher priority / extensions: compare quicr::messages types
 *   with moxygen MoQTypes object header structs when bridging streams.
 *
 * Handler bridging (design §10.4) should live next to RelayCore / MoqServerPort implementations.
 */

namespace laps::moq::shim {} // namespace laps::moq::shim
