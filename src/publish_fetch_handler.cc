// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "publish_fetch_handler.h"

namespace laps {

    PublishFetchHandler::PublishFetchHandler(const quicr::FullTrackName& full_track_name,
                                             quicr::messages::ObjectPriority priority,
                                             uint64_t subscribe_id,
                                             quicr::messages::GroupOrder group_order,
                                             uint32_t ttl,
                                             ClientManager& client_manager)
      : quicr::PublishFetchHandler(full_track_name, priority, subscribe_id, group_order, ttl)
      , client_manager_(client_manager)
    {
    }

    void PublishFetchHandler::BindForMoxygenFetch(quicr::ConnectionHandle connection_handle,
                                                  uint64_t publish_data_ctx_id)
    {
        SetConnectionId(connection_handle);
        publish_data_ctx_id_ = publish_data_ctx_id;
        SetStatus(quicr::PublishTrackHandler::Status::kOk);
        moq_fetch_bound_ = true;
    }

    quicr::PublishTrackHandler::PublishObjectStatus PublishFetchHandler::PublishObject(
      const quicr::ObjectHeaders& object_headers,
      quicr::BytesSpan data)
    {
        if (moq_fetch_bound_) {
            if (const auto moq =
                  client_manager_.TryMoxygenFetchPublishObject(static_cast<quicr::PublishFetchHandler*>(this),
                                                               object_headers,
                                                               data)) {
                return *moq;
            }
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        return quicr::PublishFetchHandler::PublishObject(object_headers, data);
    }

    quicr::PublishTrackHandler::PublishObjectStatus PublishFetchHandler::ForwardPublishedData(
      bool is_new_stream,
      uint64_t group_id,
      uint64_t subgroup_id,
      std::shared_ptr<const std::vector<uint8_t>> data)
    {
        if (moq_fetch_bound_) {
            if (const auto moq = client_manager_.TryMoxygenFetchForwardPublishedData(
                  static_cast<quicr::PublishFetchHandler*>(this),
                  is_new_stream,
                  group_id,
                  subgroup_id,
                  std::move(data))) {
                return *moq;
            }
            return quicr::PublishTrackHandler::PublishObjectStatus::kInternalError;
        }
        return quicr::PublishTrackHandler::ForwardPublishedData(
          is_new_stream, group_id, subgroup_id, std::move(data));
    }

} // namespace laps
