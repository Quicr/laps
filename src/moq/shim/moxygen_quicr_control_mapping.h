#pragma once

/**
 * @file moxygen_quicr_control_mapping.h
 * @brief P0: Field-level mapping notes for moxygen MoQTypes <-> quicr messages (control plane).
 *
 * Implementations belong in .cc files that include both moxygen/MoQTypes.h and quicr headers.
 * Draft alignment: laps moxygen build uses MoQT draft-16 (getMoqtProtocols("16", true)); libquicr
 * uses quicr::kMoqtVersion — keep versions in sync when mapping.
 *
 * -----------------------------------------------------------------------------
 * SUBSCRIBE
 * -----------------------------------------------------------------------------
 * moxygen::SubscribeRequest                    quicr:: (SubscribeReceived + attributes)
 * - requestID                                  -> messages::RequestID / uint64_t request_id
 * - fullTrackName (namespace + name tuples)    -> quicr::FullTrackName
 * - priority                                   -> subscribe attributes priority (uint8_t)
 * - groupOrder                                 -> messages::GroupOrder
 * - minStart / maxStart / startGroup / ...      -> messages::Location / filter (LocationType)
 * - filterType                                 -> messages::Filter / extensions
 * - forward                                    -> forward flag in subscribe attributes
 * - trackAlias (if present)                    -> messages::TrackAlias where applicable
 *
 * -----------------------------------------------------------------------------
 * FETCH (standalone / joining)
 * -----------------------------------------------------------------------------
 * moxygen::Fetch                               quicr:: StandaloneFetchReceived / JoiningFetchReceived
 * - requestID, fullTrackName, priority         -> same pattern as subscribe
 * - end                                        -> FetchEndLocation equivalent
 *
 * -----------------------------------------------------------------------------
 * SUBSCRIBE_NAMESPACE
 * -----------------------------------------------------------------------------
 * moxygen::SubscribeNamespace                  -> SubscribeNamespaceReceived + prefix_namespace
 * - requestID, trackNamespacePrefix, forward   -> messages::SubscribeNamespaceAttributes
 *
 * -----------------------------------------------------------------------------
 * PUBLISH_NAMESPACE / PUBLISH
 * -----------------------------------------------------------------------------
 * moxygen::PublishNamespace                    -> PublishNamespaceReceived + PublishNamespaceAttributes
 * moxygen::PublishRequest                      -> PublishReceived + messages::PublishAttributes
 *
 * -----------------------------------------------------------------------------
 * TRACK_STATUS
 * -----------------------------------------------------------------------------
 * moxygen::TrackStatus                         -> TrackStatusReceived + FullTrackName
 *
 * Error / OK replies must map RequestErrorCode / SubscribeErrorCode (moxygen) to
 * quicr::RequestResponse and back when completing coroutines from RelayCore decisions.
 */

namespace laps::moq::shim {
    // Intentionally no code: mapping is implemented alongside adapter translation units (future).
} // namespace laps::moq::shim
