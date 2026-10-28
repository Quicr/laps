# LAPS Admin API

LAPS exposes a runtime admin API over MoQ publish and subscribe tracks. An admin client publishes a JSON request to the relay and subscribes to a JSON reply on a second track. `state.sizes` reports the sizes of the relay's internal maps and the contents of the object cache. `peers` reports each peering session, whether this relay accepted it or dialed it, and how many subscribe tracks and publishes are being forwarded over it.

The relay binary is `lapsRelay`. The admin client is `lapsAdmin`.

## Why the tracks are split this way

MoQ allows one publisher for a given full namespace and track name. Several admin clients cannot publish the same `laps/admin` namespace with the name `rpc`. Each sender is encoded in the namespace, and the relay learns about those publishers with a subscribe-tracks interest on a prefix.

The relay does not know admin client names ahead of time. It subscribes to a prefix built from a configurable base and its own endpoint id. Each client appends its own endpoint id when it publishes. The reply goes the other way: the client already knows the relay endpoint id, so it subscribes to one full track that only that relay publishes.

```text
base prefix          laps/admin
relay endpoint id    relay-b
client endpoint id   admin-client-xyz

relay subscribe-tracks prefix
  laps/admin/relay-b

client request publish
  namespace  laps/admin/relay-b/admin-client-xyz
  name       rpc

client reply subscribe
  namespace  laps/admin/admin-client-xyz/relay-b
  name       rpc
```

A slash in any of those strings is a namespace tuple delimiter, including the base prefix and both endpoint ids. Empty segments are dropped. `relay/b` and `admin/client` therefore produce:

```text
request  laps/admin/relay/b/admin/client
reply    laps/admin/admin/client/relay/b
```

A namespace may contain at most 32 tuple entries. The relay refuses to start when the subscribe prefix would exceed that limit, and `lapsAdmin` refuses to connect when the request or reply namespace would.

```mermaid
sequenceDiagram
    participant Admin as lapsAdmin
    participant Relay as lapsRelay

    Admin->>Relay: QUIC + SETUP
    Relay-->>Admin: SETUP endpoint id
    Admin->>Relay: SUBSCRIBE laps/admin/admin-client-xyz/relay-b name rpc
    Admin->>Relay: PUBLISH laps/admin/relay-b/admin-client-xyz name rpc
    Note over Relay: Prefix laps/admin/relay-b matches.<br/>Publish is not paused.
    Admin->>Relay: object group 0, JSON request
    Note over Relay: Register local reply publish if needed.
    Relay-->>Admin: object, JSON result or error
```

## Relay interest

On startup the relay records a subscribe-tracks interest for `base + endpoint id`. The default base is `laps/admin`, so a relay whose `--endpoint_id` is `moq-server` listens on `laps/admin/moq-server`.

libquicr sends `SUBSCRIBE_TRACKS` only from a server-mode session, and a client-mode session rejects that message. The admin interest is therefore stored locally in `state_.subscribe_tracks` under its namespace prefix. It is not sent on the wire to connected clients. Inside that prefix the handler is keyed by the maximum `uint64` connection handle, which is not a QUIC connection.

When a publish arrives, the relay treats a subscribe-tracks prefix as a match only when the prefix is shorter than or equal to the published namespace (`TrackNamespace::IsPrefixOf`). A matching admin request is marked as having a subscriber, so the publisher is asked to forward. The local admin handler does not create a forwarding publish track and does not mirror the objects back out. Objects are consumed later on the transport thread.

A publish whose name is `rpc` and whose namespace starts with the admin base, but not with this relay's endpoint id, is outside the interest. The relay pauses that publish and logs that `--relay-endpoint` must be this relay's `--endpoint_id`.

The interest is installed in `AdminService::Start()`, after metrics setup and before the relay listens. `AdminService::Stop()` removes it.

## Request and reply objects

Both directions use stream track mode. One RPC message is one MoQ object. The payload is a single JSON value encoded as UTF-8. Payloads larger than 65536 bytes are rejected.

`lapsAdmin` sends the request as group 0, object 0, subgroup 0. The relay sends each reply as a new group on that client's reply track, with object 0 and subgroup 0. The reply group id starts at 0 and increases by one for each response to that client.

The reply track is registered the first time a request from that client is consumed. Registration uses the same local-publish path as metrics: connection handle 0, request id 0, and no session. A client may subscribe before that publish exists. The subscribe is stored in `state_.subscribes`, and the later local publish attaches the subscriber.

Admin objects are taken in `SubscribeTrackHandler::ObjectReceived` before cache and fan-out. A consumed admin object is not forwarded as media. Copies that arrive through peering (`is_from_peer_`) are left alone, so a peered request does not create a local reply publish off the transport thread.

The reply publish, the per-client group counter, and the admin consume path run on the transport thread.

## RPC messages

Every message is one JSON object. Unknown object fields are ignored. `id` may be a JSON string or a JSON number. `lapsAdmin` sends `id` as a decimal string of milliseconds since the Unix epoch. The relay copies the received `id` into the response. A response produced before an id is parsed uses an empty id.

The relay adds `api_version` to every result and every error. It is an unsigned integer and identifies the admin RPC this process speaks. The API has not been released, so every response reports version `1`. That number stays at 1 while the message shape is still changing. `lapsAdmin` is built for the same constant. After the session closes it logs the version the relay reported. A missing version, or a version other than the one this client speaks, is a warning on stderr. The JSON result is still printed after that log.

Request:

```json
{
  "id": "1793081125284",
  "method": "state.sizes",
  "params": {}
}
```

`params` is optional. When present it must be one JSON value. `state.sizes` parses `params` and does not use them.

Result:

```json
{
  "id": "1793081125284",
  "api_version": 1,
  "result": {}
}
```

Error:

```json
{
  "id": "1793081125284",
  "api_version": 1,
  "error": {
    "message": "unknown method"
  }
}
```

| Condition | `error.message` |
| --- | --- |
| Payload larger than 65536 bytes | `request exceeds 65536 bytes` |
| Body is not one JSON object | Parser reason, such as `expected an object` |
| Object has no `method` string | `request requires a method` |
| `method` is not implemented | `unknown method` |
| Handling the request throws | The exception text |

`lapsAdmin` prints the response on stdout, indented with two spaces. Empty objects and arrays stay on one line. A body that is not a single JSON value is printed as received. The process exits 0 when the body parses as a result and has no `error`. Otherwise it exits 1 and, when the error object contains `message`, prints that message on stderr.

## `state.sizes`

Method name: `state.sizes`. `lapsAdmin` accepts `state` as an alias and sends `state.sizes`.

The result is a snapshot taken while `state_.state_mutex` is held. Session and peering maps are counted under their own locks inside that hold. The object cache has no separate lock. The snapshot runs on the transport thread, which is also the thread that inserts cache entries.

`cache_tracks` remains the number of cached tracks. The same count is `cache.tracks`.

```json
{
  "id": "1793081125284",
  "api_version": 1,
  "result": {
    "relay_id": "relay-b",
    "connections": 1,
    "subscriptions": 1,
    "publishes": 2,
    "publish_namespaces": 0,
    "subscribe_namespaces": 0,
    "subscribe_tracks": 1,
    "requests": 0,
    "subscribe_active": 1,
    "publish_subscribes_by_request": 1,
    "subscribe_alias_by_request": 1,
    "connection_metrics": 2,
    "fanout_namespaces": 0,
    "notified_namespaces": 0,
    "cache_tracks": 1,
    "track_rankings": 0,
    "peering_interests": 1,
    "cache": {
      "enabled": true,
      "duration_ms": 60000,
      "interval_ms": 1000,
      "tracks": 1,
      "groups": 2,
      "objects": 3,
      "bytes": 10,
      "entries": [
        {
          "track_alias": 99,
          "track_namespace": "media/live",
          "track_name": "video",
          "groups": 2,
          "objects": 3,
          "bytes": 10,
          "oldest_group": 4,
          "newest_group": 5
        }
      ]
    }
  }
}
```

### Map sizes

| Field | Count |
| --- | --- |
| `relay_id` | This relay's `--endpoint_id`. |
| `connections` | Accepted MoQ sessions. |
| `subscriptions` | Subscriber tracks in `state_.subscribes`, keyed by track alias and subscriber connection. One connection may subscribe to a full track name once. |
| `publishes` | Publisher tracks the relay has subscribed to, in `state_.pub_subscribes`, keyed by track alias and publisher connection. Local publishes use connection handle 0. |
| `publish_namespaces` | Announced publish namespaces in `state_.pub_namespace_active`. |
| `subscribe_namespaces` | `SUBSCRIBE_NAMESPACE` prefixes in `state_.subscribe_namespaces`. |
| `subscribe_tracks` | `SUBSCRIBE_TRACKS` prefixes in `state_.subscribe_tracks`, including the relay's own admin prefix. A running relay therefore reports at least 1. |
| `requests` | In-flight namespace request transactions in `state_.requests`. |
| `subscribe_active` | Distinct tracks in `state_.subscribe_active_`, keyed by namespace and track-name hash. Each entry holds the set of subscribers for that track. The field is the number of tracks, not the number of subscribers inside those sets. |
| `publish_subscribes_by_request` | Publisher-initiated subscribes indexed by request id and connection. |
| `subscribe_alias_by_request` | Request id to track-alias mappings for subscribes received from clients. |
| `connection_metrics` | Connections with publish or subscribe counters. Connection handle 0 is included when a local publish, such as metrics or an admin reply, has incremented that counter. |
| `fanout_namespaces` | Publish-namespace handlers created to fan a namespace out to a subscriber. |
| `notified_namespaces` | Namespaces already reported with `NAMESPACE` to a subscriber. |
| `cache_tracks` | Tracks present in the object cache. Same value as `cache.tracks`. |
| `track_rankings` | Subscribe namespaces that have a `TrackRanking` instance. See [track-ranking.md](track-ranking.md). |
| `peering_interests` | Tracks for which peering has been told about a local subscriber. |

### Cache

`cache` describes the objects the relay is holding for fetch and largest-object lookup.

| Field | Description |
| --- | --- |
| `enabled` | True unless the relay was started with `--disable_cache`. |
| `duration_ms` | Cache lifetime from `--cache_duration`. The default is 60000. |
| `interval_ms` | Bucket width. Groups expire on this interval. The implementation uses 1000. |
| `tracks` | Number of track caches. |
| `groups` | Sum of groups currently stored. |
| `objects` | Sum of objects in those groups. |
| `bytes` | Sum of stored object payloads. Headers are not included. |
| `entries` | One object per cached track, sorted by namespace, name, then track alias. |

Each entry:

| Field | Description |
| --- | --- |
| `track_alias` | Cache key. This is the track alias, which libquicr sets to the full track-name hash. |
| `track_namespace` | Namespace of the matching publisher track, with `/` between tuple entries. Empty when no publisher track with that alias is in `pub_subscribes`. |
| `track_name` | Track name for that alias. Empty in the same case as `track_namespace`. |
| `groups` | Groups stored for this track. |
| `objects` | Objects stored for this track. |
| `bytes` | Payload bytes stored for this track. |
| `oldest_group` | Smallest group id still cached. Omitted when the track holds no groups. |
| `newest_group` | Largest group id still cached. Omitted when the track holds no groups. |

Reading the cache advances its time buckets, so groups that have already expired are dropped before they are counted.

## `peers`

Method name: `peers`.

The result lists the peering sessions this relay currently holds. A session this relay accepted from a remote peer has `role` `accepted`. A session this relay opened itself has `role` `dialed`. Dialed sessions stay in the list while the relay retries them, including when `status` is `disconnected`. Accepted sessions leave the list when the connection closes.

`node_id`, `contact`, and `node_type` are present after the peering handshake identifies the remote node. `contact` is that peer's relay endpoint id. `metrics.srtt_us` is the smoothed RTT in microseconds, sampled when the RPC runs. It is `0` when the connection has no sample yet.

`metrics.subscribe_tracks` is the number of tracks this relay is currently forwarding to that peer. That is the client forwarding table for the peer, plus each peer-to-peer flow sent onward to it. `metrics.publishes` is the number of tracks arriving from that peer that this relay is forwarding to a local subscriber or to another peer. Both are gauges of the forwarding tables at the moment of the RPC. A session with no connection reports `0` for both. The local client path uses session id 0 and is not a peer, so it is not included.

```json
{
  "id": "1793081125284",
  "api_version": 1,
  "result": {
    "relay_id": "relay-b",
    "peers": [
      {
        "session_id": 11,
        "role": "accepted",
        "status": "connected",
        "host": "192.0.2.10",
        "port": 33434,
        "node_id": 42,
        "contact": "relay-a",
        "node_type": "edge",
        "metrics": {
          "srtt_us": 1500,
          "subscribe_tracks": 4,
          "publishes": 2
        }
      },
      {
        "session_id": 12,
        "role": "dialed",
        "status": "connecting",
        "host": "192.0.2.20",
        "port": 33434,
        "metrics": {
          "srtt_us": 0,
          "subscribe_tracks": 0,
          "publishes": 0
        }
      }
    ]
  }
}
```

| Field | Description |
| --- | --- |
| `relay_id` | This relay's `--endpoint_id`. |
| `peers` | Accepted sessions first, then dialed sessions. Within a role, ordered by host, port, then session id. |
| `session_id` | QUIC connection id of the peering session. `0` when a dialed session has no connection. |
| `role` | `accepted` when the peer connected to this relay. `dialed` when this relay opened the connection. |
| `status` | `connecting`, `connected`, or `disconnected`. |
| `host` | Peer address. For an accepted session this is the remote address. For a dialed session this is the configured peer host. |
| `port` | Peer UDP port. |
| `node_id` | Numeric id assigned from the remote endpoint id. Omitted until the handshake identifies the peer. |
| `contact` | Remote relay endpoint id. Omitted until the handshake identifies the peer. |
| `node_type` | `edge`, `via`, or `stub`. Omitted until the handshake identifies the peer. |
| `metrics.srtt_us` | Smoothed RTT in microseconds. |
| `metrics.subscribe_tracks` | Tracks this relay is forwarding to the peer because of a subscribe. |
| `metrics.publishes` | Tracks arriving from the peer that this relay is forwarding onward. |

## `lapsRelay`

```text
lapsRelay --admin_namespace laps/admin --endpoint_id relay-b
```

| Option | Default | Role |
| --- | --- | --- |
| `--admin_namespace` | `laps/admin` | Base prefix. A slash separates tuple entries. The endpoint id is appended. |
| `--endpoint_id`, `-e` | `moq-server` | Relay endpoint id. This is the next tuple after the base prefix, and it is the id advertised in SETUP. |
| `--disable_cache` | off | Leaves `cache.enabled` false. New objects are not inserted. |
| `--cache_duration` | `60000` | Reported as `cache.duration_ms`. |

Startup logs the base prefix (`Using admin namespace prefix`) and, once the interest is installed, the full subscribe prefix and the track name `rpc` (`Admin subscribe tracks prefix`).

An empty admin namespace or an empty endpoint id prevents the relay from finishing startup. A prefix of 32 or more tuple entries does the same.

## `lapsAdmin`

```text
lapsAdmin --uri moq://127.0.0.1:33435 state
```

The client connects, reads the endpoint id from the relay SETUP, subscribes to the reply track, publishes the request track, and sends one object after the relay accepts the publish with forwarding on. It then waits for the first non-empty reply object.

| Option | Default | Role |
| --- | --- | --- |
| positional method | required | `state` or `state.sizes`. Any other string is sent as the method so an unknown-method error can be observed. |
| `--uri` | `moq://127.0.0.1:33435` | Relay URI. `33435` is the `lapsRelay` default listen port. |
| `--relay-endpoint` | SETUP endpoint id | Must equal the endpoint id in the relay SETUP. Omit it to use that id. |
| `--endpoint-id` | `admin-<pid>` | This client's endpoint id, appended to the request namespace and placed before the relay id in the reply namespace. |
| `--admin-namespace` | `laps/admin` | Same base prefix the relay was given. |
| `--params` | omitted | One JSON value sent as `params`. |
| `--timeout-ms` | `5000` | Time allowed to connect, to be allowed to publish, and to receive the reply. The QUIC idle timeout is twice this value. |
| `--debug` | off | Debug logging. |

When `--relay-endpoint` is set and differs from SETUP, the client disconnects before publishing and prints the endpoint id to use. When the relay pauses the request track, the client reports the subscribe prefix it expected (`<admin-namespace>/<relay endpoint id>`).

Example against a relay that advertises `relay-b`:

```text
lapsAdmin --uri moq://127.0.0.1:33435 --endpoint-id admin-client-xyz state
```

Example that also checks the endpoint id:

```text
lapsAdmin --uri moq://127.0.0.1:33435 \
  --relay-endpoint relay-b \
  --endpoint-id admin-client-xyz \
  --timeout-ms 8000 \
  state
```

## Authorization and scope

The admin prefix is not authenticated. Any client that can open a session and publish under that prefix can read the map sizes and cache summary.

The API is local to the relay that receives the publish. Peering subscribes are for a full track name, not a prefix, and the relay does not register an admin reply for an object that arrived from a peer. An admin client talks to the relay it connected to.

`state.sizes` and `peers` are the implemented methods. The message format accepts any method string. Further methods can use the same request and reply tracks, with optional `params` and a `result` or `error` object.

## Code

| Piece | Role |
| --- | --- |
| `src/admin_codec.h`, `src/admin_codec.cc` | Namespace layout, JSON parse and encode, `state.sizes` and `peers` encoding, pretty printer. |
| `src/admin_service.h`, `src/admin_service.cc` | Local subscribe-tracks interest, request dispatch, reply publish. |
| `src/admin_client.cc` | `lapsAdmin`. |
| `src/client_manager.cc` | Installs the interest, keeps admin publishes unpaused, collects the snapshot, registers the local reply. |
| `src/subscribe_handler.cc` | Delivers admin objects to the service before media forwarding. |
| `src/publish_namespace_handler.h` | `ForwardsPublishedTracks()` lets the admin interest skip outbound `PublishTrack`. |
| `src/main.cc` | `--admin_namespace`. |
| `test/admin_codec_test.cc` | Layout, JSON, cache encoding, and pretty-print checks. |
