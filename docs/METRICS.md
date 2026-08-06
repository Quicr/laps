# LAPS Metrics

LAPS publishes relay metrics over MoQ tracks as newline-delimited JSON. Metrics are sampled by libquicr callbacks and queued for a relay-owned metrics publishing thread, so the callback path does not publish directly.

## Transport

The metrics publisher starts when the relay starts and stops with the relay. Each `MetricsSampled()` callback serializes the sample to one JSON object plus a trailing newline and pushes it into a `quicr::SafeQueue`. The metrics thread blocks on that queue and publishes each JSON line through a relay-local publish track.

The queue is bounded. If the queue is full, the oldest queued sample is dropped and the new sample is kept.

The relay registers the metrics track as a self-published track using connection handle `0`; it does not publish metrics back through the client connection that emitted the sample. Normal `SUBSCRIBE` and `SUBSCRIBE_TRACKS` matching then forwards metrics objects to interested subscribers the same way it forwards objects from other publishers.

Metrics are forwarded as MoQ objects using stream track mode. Group IDs increase on the metrics track, with `object_id` and `subgroup_id` set to `0`.

## Tracks

The default metrics namespace is:

```text
metrics/<relay id>
```

The namespace can be overridden at startup:

```text
lapsRelay --metrics_namespace custom/metrics/namespace
```

The relay logs the namespace on startup. Slash-separated namespace strings are converted to MoQ namespace tuple entries.

All metrics types are published on one schema-versioned track name under the metrics namespace:

```text
laps.metrics.openapi.v1
```

The track name identifies the OpenAPI schema used by every JSON line. The metric variant is identified by the JSON `type` field:

| `type` | Source callback |
| --- | --- |
| `connection` | `ClientManager::MetricsSampled(connection_handle, ConnectionMetrics)` |
| `subscribe` | `PublishTrackHandler::MetricsSampled(PublishTrackMetrics)` |
| `publish` | `SubscribeTrackHandler::MetricsSampled(SubscribeTrackMetrics)` |

The `type` names the remote role the sample describes, which is the inverse of the relay-side handler that produced it. The relay runs a publish track in order to send to a subscriber, so those samples are typed `subscribe`. The relay runs a subscribe track in order to receive from a publisher, so those samples are typed `publish`.

For example, with relay ID `relay-a`, consumers can subscribe to:

```text
namespace: metrics/relay-a
name: laps.metrics.openapi.v1
```

The OpenAPI 3.1 schema is published in [`docs/metrics.openapi.yaml`](metrics.openapi.yaml). It defines a `MetricsSample` schema with a `type` discriminator over `connection`, `subscribe`, and `publish` payloads.

## Common Fields

All metrics JSON objects include:

| Field | Type | Description |
| --- | --- | --- |
| `type` | string | Metrics discriminator: `connection`, `subscribe`, or `publish`. |
| `relay_id` | string | Relay endpoint ID. |
| `sample_time_us` | unsigned integer | Sample timestamp in microseconds since Unix epoch. |
| `connection_handle` | unsigned integer | libquicr connection handle associated with the sample. |

All integer values are emitted as JSON numbers.

`connection_handle` and `remote_endpoint_id` are tags: every sample of every type carries both, and together they identify the remote peer the sample is about. `relay_id`, `track_namespace`, and `track_name` are tags as well. The remaining fields are measurements for the sample period.

## Min/Max/Average Objects

Several QUIC fields use the same min/max/average shape:

```json
{
  "min": 100,
  "max": 300,
  "avg": 180,
  "value_sum": 900,
  "value_count": 5
}
```

| Field | Description |
| --- | --- |
| `min` | Minimum observed value in the sample period. |
| `max` | Maximum observed value in the sample period. |
| `avg` | Average value in the sample period. |
| `value_sum` | Sum of all observed values in the sample period. |
| `value_count` | Number of values included in the sample period. |

## Connection Metrics

Connection metrics are emitted on the schema track with `type` set to `connection`.

Example:

```json
{"type":"connection","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"remote_endpoint_id":"client-42","remote_ip":"192.0.2.44","remote_port":54431,"publish_tracks":3,"rx_dgram_unknown_track_alias":0,"rx_dgram_invalid_type":0,"rx_dgram_decode_failed":0,"rx_stream_buffer_error":0,"rx_stream_unknown_track_alias":0,"rx_stream_invalid_type":0,"invalid_ctrl_stream_msg":0,"quic":{"cwin_congested":0,"prev_cwin_congested":0,"tx_congested":0,"tx_rate_bps":{"min":1000,"max":2000,"avg":1500,"value_sum":3000,"value_count":2},"rx_rate_bps":{"min":900,"max":1800,"avg":1350,"value_sum":2700,"value_count":2},"tx_cwin_bytes":{"min":12000,"max":16000,"avg":14000,"value_sum":28000,"value_count":2},"tx_in_transit_bytes":{"min":0,"max":8000,"avg":4000,"value_sum":8000,"value_count":2},"rtt_us":{"min":1000,"max":1400,"avg":1200,"value_sum":2400,"value_count":2},"srtt_us":{"min":1100,"max":1300,"avg":1200,"value_sum":2400,"value_count":2},"tx_retransmits":0,"tx_lost_pkts":0,"tx_timer_losses":0,"tx_spurious_losses":0,"rx_dgrams":12,"rx_dgrams_bytes":4096,"tx_dgram_cb":15,"tx_dgram_ack":14,"tx_dgram_lost":0,"tx_dgram_spurious":0,"tx_dgram_drops":0}}
```

Connection-specific fields:

| Field | Description |
| --- | --- |
| `remote_endpoint_id` | Remote endpoint ID from the client's `CLIENT_SETUP`. Empty until setup is received. |
| `remote_ip` | Remote client IP address recorded when the connection was accepted. Empty if unavailable. |
| `remote_port` | Remote client UDP port recorded when the connection was accepted. `0` if unavailable. |
| `publish_tracks` | Number of active publish tracks for this connection. |
| `rx_dgram_unknown_track_alias` | Datagrams received for an unknown track alias. |
| `rx_dgram_invalid_type` | Datagrams with an invalid object datagram type. |
| `rx_dgram_decode_failed` | Datagrams that failed to decode. |
| `rx_stream_buffer_error` | Stream buffer errors while parsing received stream data. |
| `rx_stream_unknown_track_alias` | Stream data received for an unknown track alias. |
| `rx_stream_invalid_type` | Stream data with an invalid message type. |
| `invalid_ctrl_stream_msg` | Invalid control stream messages. This should normally be `0`. |
| `quic` | QUIC connection metrics object. |

Connection `quic` fields:

| Field | Description |
| --- | --- |
| `cwin_congested` | Count of times congestion window was low or zero. |
| `prev_cwin_congested` | Previous congestion-window congestion count. |
| `tx_congested` | Count of times transmit path was considered congested. |
| `tx_rate_bps` | Transmit rate in bits per second. |
| `rx_rate_bps` | Receive rate estimate in bits per second. |
| `tx_cwin_bytes` | Congestion window bytes. |
| `tx_in_transit_bytes` | Bytes in transit. |
| `rtt_us` | RTT in microseconds. |
| `srtt_us` | Smoothed RTT in microseconds. |
| `tx_retransmits` | Retransmission count. |
| `tx_lost_pkts` | Lost packet count. |
| `tx_timer_losses` | Packet losses detected by timer. |
| `tx_spurious_losses` | Packets marked lost that were later acknowledged. |
| `rx_dgrams` | Received datagram count. |
| `rx_dgrams_bytes` | Received datagram bytes. |
| `tx_dgram_cb` | Datagram send callback count. |
| `tx_dgram_ack` | Acknowledged datagram callback count. |
| `tx_dgram_lost` | Lost datagram callback count. |
| `tx_dgram_spurious` | Late or delayed datagram acknowledgement count. |
| `tx_dgram_drops` | Datagrams dropped because the data context was missing. |

`tx_rate_bps`, `rx_rate_bps`, `tx_cwin_bytes`, `tx_in_transit_bytes`, `rtt_us`, and `srtt_us` use the min/max/average object shape.

## Subscribe Metrics

Subscribe metrics are emitted on the schema track with `type` set to `subscribe`. They are sampled on the relay publish track that sends to the subscriber, so one sample is produced per subscriber connection per track.

Example:

```json
{"type":"subscribe","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"remote_endpoint_id":"client-42","track_namespace":"media/live/event1","track_name":"video","bytes":16384,"objects":64,"objects_dropped_not_ok":0,"quic":{"tx_buffer_drops":0,"tx_queue_discards":0,"tx_queue_expired":0,"tx_delayed_callback":0,"tx_reset_wait":0,"tx_queue_size":{"min":0,"max":3,"avg":1,"value_sum":6,"value_count":4},"tx_callback_ms":{"min":0,"max":2,"avg":1,"value_sum":4,"value_count":4},"tx_object_duration_us":{"min":100,"max":500,"avg":250,"value_sum":1000,"value_count":4}}}
```

Subscribe-specific fields:

| Field | Description |
| --- | --- |
| `remote_endpoint_id` | Remote endpoint ID from the subscribing client's `CLIENT_SETUP`. Empty until setup is received. |
| `track_namespace` | Namespace of the content track. |
| `track_name` | Name of the content track. |
| `bytes` | Payload bytes sent to the subscriber during the sample period. |
| `objects` | Objects sent to the subscriber during the sample period. |
| `objects_dropped_not_ok` | Objects dropped because the publish handler was not in a publishable state. |
| `quic` | QUIC data-context metrics object. |

Subscribe `quic` fields:

| Field | Description |
| --- | --- |
| `tx_buffer_drops` | Write-buffer drops due to reset handling. |
| `tx_queue_discards` | Objects discarded due to queue clearing or stream transition. |
| `tx_queue_expired` | Objects expired before transmit. |
| `tx_delayed_callback` | Count of delayed transmit callbacks. |
| `tx_reset_wait` | Count of reset-and-wait events. |
| `tx_queue_size` | Transmit queue size during the sample period. |
| `tx_callback_ms` | Transmit callback duration in milliseconds. |
| `tx_object_duration_us` | Object time in queue in microseconds. |

`tx_queue_size`, `tx_callback_ms`, and `tx_object_duration_us` use the min/max/average object shape.

## Publish Metrics

Publish metrics are emitted on the schema track with `type` set to `publish`. They are sampled on the relay subscribe track that receives from the publisher, so one sample is produced per publisher connection per track.

Example:

```json
{"type":"publish","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"remote_endpoint_id":"client-42","track_namespace":"media/live/event1","track_name":"video","bytes":8192,"objects":32,"subscribers":4}
```

Publish-specific fields:

| Field | Description |
| --- | --- |
| `remote_endpoint_id` | Remote endpoint ID from the publishing client's `CLIENT_SETUP`. Empty until setup is received. |
| `track_namespace` | Namespace of the content track. |
| `track_name` | Name of the content track. |
| `bytes` | Payload bytes received from the publisher during the sample period. |
| `objects` | Objects received from the publisher during the sample period. |
| `subscribers` | Current number of local fanout subscribers the relay is serving from this publisher's track. Subscribers matched through a subscribe namespace are not counted. |
