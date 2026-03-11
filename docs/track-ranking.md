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
    PNH -->|"Only top 3 tracks"| S
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
| **TrackRanking** | Aggregates property values from all matching tracks, maintains sorted order by (property_type, property_value), and notifies `PublishNamespaceHandler` instances when the top-N list changes. |
| **PublishNamespaceHandler** | Receives the ranked track list, maintains `active_tracks_` (max N tracks), and promotes/demotes tracks by calling `PublishTrack` / `UnPublishTrack` on the underlying transport. |
| **SubscribeTrackHandler** | Receives objects from publishers, extracts property values from extensions, pushes updates to `TrackRanking`, and fans out objects to subscribe namespaces. |

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
        FL["flat_track_list_<br/>sorted track aliases"]
        NH["ns_handlers_<br/>PublishNamespaceHandler refs"]
    end

    subgraph PublishNamespaceHandler["PublishNamespaceHandler (per subscriber conn)"]
        AT["active_tracks_<br/>max N tracks"]
        H["handlers_<br/>track alias → PublishTrackHandler"]
    end

    subgraph SubscribeTrackHandler["SubscribeTrackHandler (per published track)"]
        TPV["tracked_properties_value_"]
        SNS["sub_namespaces_<br/>fanout targets"]
    end

    SubscribeTrackHandler -->|"UpdateValue(ta, prop, value, tick)"| TrackRanking
    TrackRanking -->|"UpdateTrackRanking(flat_track_list)"| PublishNamespaceHandler
    PublishNamespaceHandler -->|"PublishTrack / UnPublishTrack"| Transport
    ClientManager --> TrackRanking
    ClientManager --> PublishNamespaceHandler
```

## Data Flow: Subscribe Namespace Received

When a client sends `SUBSCRIBE_NAMESPACE` for a prefix:

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
        SubscribeTrackHandler->>TrackRanking: UpdateValue(ta, prop, value, tick)
        TrackRanking->>TrackRanking: Update ordered_tracks_, flat_track_list_
        loop For each ns_handler
            TrackRanking->>PublishNamespaceHandler: UpdateTrackRanking(flat_track_list)
            PublishNamespaceHandler->>PublishNamespaceHandler: Update active_tracks_ (top-N)
            alt New track in top-N
                PublishNamespaceHandler->>Transport: PublishTrack(handler)
            end
            alt Track dropped from top-N
                PublishNamespaceHandler->>Transport: UnPublishTrack(handler)
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
  - Value: `TrackEntry` = `map<TrackAlias, tick>` — tracks in that bucket, with last update tick
- **flat_track_list_**: Sorted list of `(TrackAlias, tick)` for the selected property
  - Sorted by: property value **descending**, then tick **ascending** (newer updates rank higher within same value)

### Sort Order

```mermaid
flowchart LR
    subgraph Input["Incoming Updates"]
        A["Track A: prop=12, value=2"]
        B["Track B: prop=12, value=1"]
        C["Track C: prop=12, value=2"]
    end

    subgraph Sorted["flat_track_list_ (desc value, asc tick)"]
        R1["1. Track A (value=2)"]
        R2["2. Track C (value=2)"]
        R3["3. Track B (value=1)"]
    end

    Input --> Sorted
```

Higher property values rank first. Within the same value, lower tick (older) comes first; the code uses reverse iteration so more recent ticks rank higher when values are equal.

### Top-N Selection

**PublishNamespaceHandler** keeps at most `max_tracks_selected_` (default 3) tracks active:

- When `UpdateTrackRanking` is called with the new `flat_track_list_`, it takes the first N entries
- Tracks not in the top-N are removed from `active_tracks_` after a grace period (`delay_publish_done_ms`)
- New tracks entering the top-N trigger `PublishTrack`; demoted tracks trigger `UnPublishTrack`

## Lifecycle: Connection and Namespace Cleanup

```mermaid
flowchart TB
    A[UnsubscribeNamespaceReceived] --> B[Remove PublishNamespaceHandler from subscribe namespaces]
    B --> C[track_rankings_->RemoveNamespaceHandler]
    C --> D[SubscribeTrackHandler->RemoveSubscribeNamespace]
    D --> E{Last subscriber for namespace?}
    E -->|Yes| F[track_rankings_.erase]
    E -->|No| G[Continue]
```

## Configuration Parameters

| Parameter | Component | Default | Description |
|-----------|-----------|---------|-------------|
| `max_tracks_selected_` | PublishNamespaceHandler | 3 | Maximum number of tracks to forward per subscribe namespace |
| `inactive_age_ms_` | Both | 3000 / 5000 | Age (ms) after which a track is considered stale |
| `delay_publish_done_ms` | PublishNamespaceHandler | 500 | Grace period before unpublishing a demoted track |
| `kRefreshRankingIntervalMs` | SubscribeTrackHandler | 1000 | Minimum interval between ranking updates for the same track |

## Property Extraction

The **SubscribeTrackHandler** tracks property values from object extensions:

- Properties are read from `object_headers.extensions` and `object_headers.immutable_extensions`
- Currently, property `12` is tracked (see `tracked_properties_value_.emplace(12, 0)` in constructor)
- Only even-numbered properties are updated (see `prop % 2 != 0` check)
- Updates are throttled by `kRefreshRankingIntervalMs` to avoid excessive ranking churn

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
        S7[PublishNamespaceHandler: Select top-N, PublishTrack/UnPublishTrack]
        S8[Subscriber receives only top-N tracks]
    end

    S1 --> S2 --> S3 --> S4 --> S5 --> S6 --> S7 --> S8
```

Track ranking enables efficient **Top-N** delivery: when many tracks match a subscribe namespace, the relay forwards only the top N by a chosen property, reducing bandwidth and focusing the subscriber on the most relevant streams.
