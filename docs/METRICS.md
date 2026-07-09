# LAPS Metrics

LAPS publishes relay metrics over MoQ tracks as newline-delimited JSON. Metrics are sampled by libquicr callbacks and queued for a relay-owned metrics publishing thread, so the callback path does not publish directly.

## Transport

The metrics publisher starts when the relay starts and stops with the relay. Each `MetricsSampled()` callback serializes the sample to one JSON object plus a trailing newline and pushes it into a `quicr::SafeQueue`. The metrics thread blocks on that queue and publishes each JSON line with `PublishTrack`.

The queue is bounded. If the queue is full, the oldest queued sample is dropped and the new sample is kept.

Metrics are published as MoQ objects using datagram track mode. Object IDs increase per metrics track, with `group_id` set to `0`.

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

Each metrics type is published on a separate track name under the metrics namespace:

| Track name | Source callback |
| --- | --- |
| `connection` | `ClientManager::MetricsSampled(connection_handle, ConnectionMetrics)` |
| `subscribe` | `SubscribeTrackHandler::MetricsSampled(SubscribeTrackMetrics)` |
| `publish` | `PublishTrackHandler::MetricsSampled(PublishTrackMetrics)` |

For example, with relay ID `relay-a`, consumers can subscribe to:

```text
namespace: metrics/relay-a
name: connection
```

## Common Fields

All metrics JSON objects include:

| Field | Type | Description |
| --- | --- | --- |
| `type` | string | Metrics type: `connection`, `subscribe`, or `publish`. |
| `relay_id` | string | Relay endpoint ID. |
| `sample_time_us` | unsigned integer | Sample timestamp in microseconds since Unix epoch. |
| `connection_handle` | unsigned integer | libquicr connection handle associated with the sample. |

All integer values are emitted as JSON numbers.

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

Connection metrics are emitted on track name `connection`.

Example:

```json
{"type":"connection","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"remote_ip":"192.0.2.44","remote_port":54431,"publish_tracks":3,"rx_dgram_unknown_track_alias":0,"rx_dgram_invalid_type":0,"rx_dgram_decode_failed":0,"rx_stream_buffer_error":0,"rx_stream_unknown_track_alias":0,"rx_stream_invalid_type":0,"invalid_ctrl_stream_msg":0,"quic":{"cwin_congested":0,"prev_cwin_congested":0,"tx_congested":0,"tx_rate_bps":{"min":1000,"max":2000,"avg":1500,"value_sum":3000,"value_count":2},"rx_rate_bps":{"min":900,"max":1800,"avg":1350,"value_sum":2700,"value_count":2},"tx_cwin_bytes":{"min":12000,"max":16000,"avg":14000,"value_sum":28000,"value_count":2},"tx_in_transit_bytes":{"min":0,"max":8000,"avg":4000,"value_sum":8000,"value_count":2},"rtt_us":{"min":1000,"max":1400,"avg":1200,"value_sum":2400,"value_count":2},"srtt_us":{"min":1100,"max":1300,"avg":1200,"value_sum":2400,"value_count":2},"tx_retransmits":0,"tx_lost_pkts":0,"tx_timer_losses":0,"tx_spurious_losses":0,"rx_dgrams":12,"rx_dgrams_bytes":4096,"tx_dgram_cb":15,"tx_dgram_ack":14,"tx_dgram_lost":0,"tx_dgram_spurious":0,"tx_dgram_drops":0}}
```

Connection-specific fields:

| Field | Description |
| --- | --- |
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

Subscribe metrics are emitted on track name `subscribe`.

Example:

```json
{"type":"subscribe","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"track_namespace":"media/live/event1","track_name":"video","bytes_received":8192,"objects_received":32,"subscribers":4}
```

Subscribe-specific fields:

| Field | Description |
| --- | --- |
| `track_namespace` | Namespace of the subscribed content track. |
| `track_name` | Name of the subscribed content track. |
| `bytes_received` | Payload bytes received during the sample period. |
| `objects_received` | Objects received during the sample period. |
| `subscribers` | Current number of local fanout subscribers for the track. |

## Publish Metrics

Publish metrics are emitted on track name `publish`.

Example:

```json
{"type":"publish","relay_id":"relay-a","sample_time_us":1720000000000000,"connection_handle":10,"track_namespace":"media/live/event1","track_name":"video","bytes_published":16384,"objects_published":64,"objects_dropped_not_ok":0,"quic":{"tx_buffer_drops":0,"tx_queue_discards":0,"tx_queue_expired":0,"tx_delayed_callback":0,"tx_reset_wait":0,"tx_queue_size":{"min":0,"max":3,"avg":1,"value_sum":6,"value_count":4},"tx_callback_ms":{"min":0,"max":2,"avg":1,"value_sum":4,"value_count":4},"tx_object_duration_us":{"min":100,"max":500,"avg":250,"value_sum":1000,"value_count":4}}}
```

Publish-specific fields:

| Field | Description |
| --- | --- |
| `track_namespace` | Namespace of the published content track. |
| `track_name` | Name of the published content track. |
| `bytes_published` | Payload bytes published during the sample period. |
| `objects_published` | Objects published during the sample period. |
| `objects_dropped_not_ok` | Objects dropped because the publish handler was not in a publishable state. |
| `quic` | QUIC data-context metrics object. |

Publish `quic` fields:

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

