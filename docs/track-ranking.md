# Track Ranking with Subscribe and Publish Namespaces

This document describes how track ranking works in the LAPS relay, enabling **Top-N** selection when a subscriber uses a **subscribe namespace** to receive only the highest-ranked tracks from a set of publishers.

## Overview

When a client subscribes to a **namespace prefix** (e.g., `quicr://example.com/live/`) rather than a specific track, the relay may receive many published tracks that match that prefix. Track ranking allows the relay to select and forward only the **top N** tracks based on a configurable property (e.g., quality score, bitrate, viewer count) extracted from object extensions.

```mermaid
flowchart TB
    subgraph Publishers["Publishers (many tracks)"]
        P1[Publisher 1\nTrack A]
        P2[Publisher 2\nTrack B]
        P3[Publisher 3\nTrack C]
        P4[Publisher 4\nTrack D]
    end

    subgraph Relay["LAPS Relay"]
        TR[TrackRanking]
        PNH[PublishNamespaceHandler]
    end

    subgraph Subscriber["Subscriber"]
        S[Subscribe Namespace\nquicr://example.com/live/]
    end

    P1 & P2 & P3 & P4 -->|"Objects with property values"| TR
    TR -->|"Top-N ranked list"| PNH
    PNH -->|"Only top-N tracks"| S
```

## Key Components

### Subscribe Namespace vs Publish Namespace

| Concept | Description |
|--------|-------------|
| **Subscribe Namespace** | A client subscribes to a namespace *prefix* (e.g., `quicr://example.com/live/`). The relay will forward matching tracks. One `PublishNamespaceHandler` is created per subscriber connection for this namespace. |
| **Publish Namespace** | Publishers announce tracks within namespaces. When a track's namespace matches a subscribe namespace prefix, the track is eligible for ranking and forwarding. |

```mermaid
flowchart LR
    subgraph SubscribeSide["Subscribe Side"]
        SN[Subscribe Namespace\nprefix match]
        PNH[PublishNamespaceHandler\nper subscriber conn]
    end

    subgraph PublishSide["Publish Side"]
        PN[Publish Namespace\nannounced by publisher]
        STH[SubscribeTrackHandler\nper published track]
    end

    PN -->|"prefix match"| SN
    SN --> PNH
    STH -->|"feeds ranking"| PNH
```

### Component Roles

| Component | Responsibility |
|-----------|-----------------|
| **TrackRanking** | Aggregates property values from all matching tracks (with connection ID), maintains sorted order by (property_type, property_value), removes inactive tracks, and notifies `PublishNamespaceHandler` instances when the top-N list changes. |
| **PublishNamespaceHandler** | Receives the ranked track list, filters self-tracks, maintains `published_tracks_` (max N active), and promotes/demotes tracks via `PublishTrack` or `SetStatus(kPaused)`. |
| **SubscribeTrackHandler** | Receives objects from publishers, extracts property values from extensions, pushes updates to `TrackRanking` (including connection ID), and fans out objects to subscribe namespaces. |

## Architecture Diagram

```mermaid
flowchart TB
    subgraph ClientManager["ClientManager"]
        direction TB
        TR_map["track_rankings_<br/>(namespace_hash → TrackRanking)"]
        sub_map["state_.subscribes_namespaces<br/>(namespace → conn → PublishNamespaceHandler)"]
    end

    subgraph TrackRanking["TrackRanking (per subscribe namespace)"]
        OT["ordered_tracks_<br/>(prop, value) → TrackEntry"]
        FL["flat_track_list_<br/>(alias, seq, tick, conn_id)"]
        NH["ns_handlers_<br/>ns_hash → conn_id → handler"]
    end

    subgraph PublishNamespaceHandler["PublishNamespaceHandler (per subscriber conn)"]
        PT["published_tracks_<br/>track alias → ActiveTrack"]
        H["handlers_<br/>track alias → PublishTrackHandler"]
    end

    subgraph SubscribeTrackHandler["SubscribeTrackHandler (per published track)"]
        TPV["tracked_properties_value_"]
        SNS["sub_namespaces_<br/>fanout targets"]
    end

    SubscribeTrackHandler -->|"UpdateValue(ta, prop, value, tick, conn_id)"| TrackRanking
    TrackRanking -->|"UpdateTrackRanking(flat_track_list)"| PublishNamespaceHandler
    PublishNamespaceHandler -->|"PublishTrack / SetStatus(kPaused)"| Transport
    ClientManager --> TrackRanking
    ClientManager --> PublishNamespaceHandler
```

## Data Flow: Subscribe Namespace Received

When a client sends `SUBSCRIBE_NAMESPACE` for a prefix, all matching published tracks are initially added to the handler via `PublishTrack`. Top-N selection occurs when `UpdateTrackRanking` runs (triggered by objects with property extensions).

```mermaid
sequenceDiagram
    participant Client
    participant ClientManager
    participant TrackRanking
    participant PublishNamespaceHandler
    participant Transport

    Client->>ClientManager: SUBSCRIBE_NAMESPACE(prefix)
    ClientManager->>PublishNamespaceHandler: Create(prefix, tick_service)
    ClientManager->>Transport: PublishNamespace(conn, handler)
    ClientManager->>TrackRanking: try_emplace(namespace_hash)
    ClientManager->>TrackRanking: AddNamespaceHandler(handler)
    ClientManager->>ClientManager: Match existing PUBLISH tracks
    loop For each matching track
        ClientManager->>PublishNamespaceHandler: PublishTrack(pub_handler)
        ClientManager->>SubscribeTrackHandler: AddSubscribeNamespace(handler)
        ClientManager->>SubscribeTrackHandler: SetTrackRanking(track_ranking)
    end
```

## Data Flow: Object Received and Ranking Update

When a published track receives an object with extensions:

```mermaid
sequenceDiagram
    participant Publisher
    participant SubscribeTrackHandler
    participant TrackRanking
    participant PublishNamespaceHandler
    participant Subscriber

    Publisher->>SubscribeTrackHandler: ObjectReceived(extensions)
    SubscribeTrackHandler->>SubscribeTrackHandler: UpdateTrackedProperties(extensions)
    alt Property value changed or refresh interval elapsed
        SubscribeTrackHandler->>TrackRanking: UpdateValue(ta, prop, value, tick, conn_id)
        TrackRanking->>TrackRanking: Update ordered_tracks_, flat_track_list_<br/>Remove inactive tracks
        loop For each ns_handler (per connection)
            TrackRanking->>PublishNamespaceHandler: UpdateTrackRanking(flat_track_list)
            PublishNamespaceHandler->>PublishNamespaceHandler: Filter self-tracks, select top-N
            alt New track in top-N (was paused)
                PublishNamespaceHandler->>Transport: PublishTrack(handler)
            end
            alt Track dropped from top-N
                PublishNamespaceHandler->>PublishNamespaceHandler: SetStatus(kPaused)
            end
        end
    end
    SubscribeTrackHandler->>PublishNamespaceHandler: PublishObject / ForwardPublishedData
    PublishNamespaceHandler->>Subscriber: Forward to subscriber
```

## Ranking Algorithm

### Data Structures

**TrackRanking** maintains:

- **ordered_tracks_**: `map<(PropertyType, PropertyValue), TrackEntry>`
  - Key: `(property_type, property_value)` — e.g., `(12, 2)` for property 12 with value 2
  - Value: `TrackEntry` = `map<TrackAlias, TrackTickInfo>` where `TrackTickInfo` has `insert_seq_num` (update order) and `latest_tick`
- **flat_track_list_**: Sorted list of `(TrackAlias, insert_seq_num, latest_tick, conn_id)` for the selected property
  - Sorted by: property value **descending** (via reverse map iteration), then **ascending** `insert_seq_num` (earlier updates rank higher), then **descending** `track_alias` (tie breaker)
- **track_connections_**: Maps each track alias to its publisher's connection ID (used for self-track filtering)
- **ns_handlers_**: `map<namespace_hash, map<connection_id, PublishNamespaceHandler>>` — multiple handlers per namespace (one per subscriber connection)

### Sort Order

```mermaid
flowchart LR
    subgraph Input["Incoming Updates"]
        A["Track A: prop=12, value=2\ninsert_seq=1"]
        B["Track B: prop=12, value=1"]
        C["Track C: prop=12, value=2\ninsert_seq=2"]
    end

    subgraph Sorted["flat_track_list_ (desc value, asc insert_seq)"]
        R1["1. Track A (value=2, seq=1)"]
        R2["2. Track C (value=2, seq=2)"]
        R3["3. Track B (value=1)"]
    end

    Input --> Sorted
```

Higher property values rank first. Within the same value bucket, tracks are sorted by ascending `insert_seq_num` (earlier updates rank higher), then descending `track_alias` as a tie breaker.

### Sequence Number Assignment

When a track's property value **increases** (moves to a higher-value bucket), it receives a new sequence number calculated as:
```
seq_num = (1ULL << 63) | update_value_seq_num
```

When a track's property value **decreases** (moves to a lower-value bucket), it receives an inverted sequence number calculated as:
```
seq_num = std::numeric_limits<uint64_t>::max() - update_value_seq_num
```

This ensures that when a track improves (increases value), it ranks at the **top** of the new value bucket (lower sequence number = better rank). Conversely, when a track decreases in value, it ranks at the **bottom** of its new bucket, making room for other tracks with better values to be selected.

### Top-N Selection

**PublishNamespaceHandler** keeps at most `max_tracks_selected_` (default 1) tracks active:

- When `UpdateTrackRanking` is called with the new `flat_track_list_`, it iterates the list and selects the first N entries
- **Self-track filtering**: Tracks where `publisher_conn_id == GetConnectionId()` are skipped — a subscriber does not receive their own published tracks
- Tracks must exist in `published_tracks_` (added via `PublishTrack`) before they can be selected
- New tracks entering the top-N (previously paused) trigger `PublishTrack`; demoted tracks are set to `Status::kPaused`

### Inactive Track Removal

**TrackRanking** removes stale tracks from `ordered_tracks_` during every `UpdateValue` call when `tick - latest_tick > inactive_age_ms_`. This prevents tracks that have stopped sending updates from occupying ranking slots.

Stale track removal iterates through all property value buckets and removes any track entries whose latest tick exceeds the configured age threshold. The entire `flat_track_list_` is then rebuilt if any removals occur.

## Lifecycle: Connection and Namespace Cleanup

```mermaid
flowchart TB
    A[UnsubscribeNamespaceReceived] --> B[For each matching published track]
    B --> C[SubscribeTrackHandler->RemoveSubscribeNamespace]
    B --> D[TrackRanking->RemoveNamespaceHandler]
    C --> E[Erase from subscribes_namespaces]
    D --> E
    E --> F{Last subscriber for namespace?}
    F -->|Yes| G[track_rankings_.erase]
    F -->|No| H[Continue]
```

## Configuration Parameters

| Parameter | Component | Default | Description |
|-----------|-----------|---------|-------------|
| `max_tracks_selected_` | PublishNamespaceHandler | 1 | Maximum number of tracks to forward per subscribe namespace |
| `max_tracks_selected_` | TrackRanking | 32 | Max tracks in candidate pool (used by ranking logic) |
| `inactive_age_ms_` | TrackRanking | 1500 | Age (ms) after which a track is removed from ranking |
| `inactive_age_ms_` | PublishNamespaceHandler | 3000 | Age (ms) for staleness checks |
| `delay_publish_done_ms_` | PublishNamespaceHandler | 900 | Grace period before unpublishing a demoted track |
| `kRefreshRankingIntervalMs` | SubscribeTrackHandler | 1000 | Minimum interval between ranking updates for the same track |

## Implementation Details

### TrackRanking::UpdateValue Flow

The `UpdateValue` method performs several steps in sequence:

1. **Increment sequence counter**: Increments `update_value_seq_num_` for globally unique ordering
2. **Check for bucket migration**: Scans existing property buckets to see if the track exists in a different value bucket
3. **Mark bucket changes**: If track exists in a different bucket, it's removed and `value_decreased` flag tracks whether value went down
4. **Assign sequence number**:
   - If value increased: `seq_num = (1ULL << 63) | update_value_seq_num_` (high bit set for new values)
   - If value decreased: `seq_num = std::numeric_limits<uint64_t>::max() - update_value_seq_num_` (inverted for demotion)
5. **Insert/update track**: Adds track to new value bucket or updates `latest_tick` if already present
6. **Store connection ID**: Records the publisher's connection ID for self-track filtering
7. **Remove inactive tracks**: Scans all buckets and removes tracks with `tick - latest_tick > inactive_age_ms_`
8. **Rebuild flat list**: If buckets changed, reconstructs `flat_track_list_` by iterating buckets in descending value order, then sorting by `insert_seq_num` (ascending) and `track_alias` (descending)
9. **Notify handlers**: Updates all active `PublishNamespaceHandler` instances with the new ranked list

### Weak Pointer Management

The `ns_handlers_` map stores `std::weak_ptr<PublishNamespaceHandler>` to allow handlers to be garbage collected when subscribers disconnect. During each update:
- Weak pointers are locked to check if the handler still exists
- Dead handlers are automatically erased from the map
- If all handlers for a namespace are removed, the namespace entry is deleted

## Implementation: flat_track_list_ Optimization

The `flat_track_list_` is built from `ordered_tracks_` to optimize lookups and top-N selection:

```cpp
// Iterate property value buckets in reverse order (highest value first)
for (auto it = std::make_reverse_iterator(last);
     it != std::make_reverse_iterator(first); ++it) {

    // For each value bucket, collect all tracks
    std::vector<std::tuple<TrackAlias, uint64_t, uint64_t, uint64_t>> sort_tracks;
    for (const auto& [ta, tick_info] : entry) {
        sort_tracks.emplace_back(
          ta, tick_info.insert_seq_num, tick_info.latest_tick, track_connections_[ta]);
    }

    // Sort by insert_seq_num (ascending), then track_alias (descending)
    std::ranges::sort(sort_tracks, [](const auto& a, const auto& b) {
        if (std::get<1>(a) != std::get<1>(b))
            return std::get<1>(a) < std::get<1>(b);
        return std::get<0>(a) > std::get<0>(b);
    });

    // Append sorted tracks to flat list
    flat_track_list_.insert(flat_track_list_.end(),
                            sort_tracks.begin(), sort_tracks.end());
}
```

This results in a flat list where tracks are naturally ordered by:
1. Property value (descending) — higher values first
2. Sequence number within value (ascending) — earlier/better updates first
3. Track alias (descending) — tie breaker

When `flat_track_list_` is not rebuilt (simple tick updates for existing tracks), it's updated in-place by scanning for the track and updating its `latest_tick` and `connection_id`.

## Summary

```mermaid
flowchart TB
    subgraph EndToEnd["End-to-End Flow"]
        direction TB
        S1[Subscriber: SUBSCRIBE_NAMESPACE]
        S2[Relay: Create TrackRanking + PublishNamespaceHandler]
        S3[Publishers: Publish tracks in matching namespace]
        S4[Objects arrive with property extensions]
        S5[SubscribeTrackHandler → TrackRanking.UpdateValue]
        S6[TrackRanking → PublishNamespaceHandler.UpdateTrackRanking]
        S7[PublishNamespaceHandler: Select top-N, PublishTrack/SetStatus to kPaused]
        S8[Subscriber receives only top-N tracks]
    end

    S1 --> S2 --> S3 --> S4 --> S5 --> S6 --> S7 --> S8
```

Track ranking enables efficient **Top-N** delivery: when many tracks match a subscribe namespace, the relay forwards only the top N by a chosen property, reducing bandwidth and focusing the subscriber on the most relevant streams.
