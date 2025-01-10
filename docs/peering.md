# LAPS Peering Protocol and Architecture

LAPS peering architecture enables peering between relays and clients supporting granular traffic engineering and optimizations. Peering can be simple mesh style or it can be highly scalable for a large infrastructure by using
 on-demand Via peering sessions to aggregate traffic flows in the most efficient way to provide low loss/latency
fan-out delivery.

Peering sessions support administrative policy constraints to ensure that traffic is
forwarded via a traffic engineered path using one or more relays in the network. Traffic engineered paths
can be human or AI defined to provide low duplication and even load distribution
while maintaining administrative policies.  For example, AI can optimize relay forwarding to support different classes
of service (e.g., gold, silver, bronze) while also supporting geographic region constraints. IP forwarding paths 
are a factor in maintaining administrative policies. For example, if forwarding between relay A in New York to
relay B in Seattle would IP packet forward via Canada, AI would find another relay to ensure IP forwarding
path compliance with the administrative policy. 

Peering scales to a global network of relays supporting scale into hundreds of millions of active
tracks with any combination of publishers and subscribers.


## Relay Types

The relay type is configured upon start of the relay process. A relay can change its type at runtime, but it must
disconnect all peers and reconnect them to synchronize the type. The relay type is advertised in [node information](#node-information) upon connection establishment. 

Not all relays need to perform the same functions. There are also specific use-cases for relays based on their
position in the topology and how they are used. 


Relays are *therefore* categorized into the following types:

### Via

A Via relay is used by Edge relays to forward data between other Edge relays. A Via relay is an intermediate relay that
does not participate in all the [Information Base](#information-bases) exchanges. Specifically, it only needs to participate in node information base exchanges. 

These relays have high vertical scale due to the lower number of QUIC connections between other Edge/Via relays and because
of [source routing](#source-routing) and no MoQ client connections. 

> [!NOTE]
> A Via is not intended to be burdened by the global state of subscriptions... but a Via can be configured
> to reflect [subscribe information](#subscribe-information) when [control peering](#1-control-peering) servers are not used or available. The Via in this case merely stores and propagates subscriptions. 

**Via relay operations:**

- Does not need to be part of the `announce` and `subscribe` advertisements and withdrawals selection
  for data-plane forwarding
- It participates in the `node` advertisements and withdrawals to maintain reachability information to all nodes.
- `announce` and `subscribe` advertisements/withdrawals are state maintained, but only to prevent looping and
  duplication. The retention for this state is set to the maximum convergence time. In this sense, it is a cache
  of seen announces and subscribes in the last several seconds.
- Forwards data streams and datagram objects based on [source routing](#source-routing) using the node information base.
- Traditionally has high vertical scale to support large number of tracks and bandwidth
- Strategically placed geographically near Edge relays to provide an aggregation point for fan-out of many Edge relays.
- Used by Edge relays to form a topology to a set of subscriber relays

Peering can go directly to Via or other Edge relays, depending on the [selection algorithm](#selection-algorithm),
which takes into account load, path RTT, path loss, bandwidth, administrative/business policies, etc.

```mermaid
---
title: Fan-out using Via relays
---
flowchart TD
    subgraph Seattle/WA
        P([Publisher]) --> STUB1(Home STUB)
        STUB1 --> E1[Edge 1]
    end

    subgraph San Jose/CA
        E1 --> sjE[Edge 1]
        sjE --> sjS3([Subscriber 3])
        sjE --> sjS2([Subscriber 2])
        sjE --> sjS1([Subscriber 1])
    end

    subgraph New York/NY
        E1 --> vNY[Via Relay]
        vNY --> vNYe2[Edge 2]
        vNYe2 --> nyS6([Subscriber 6])
        vNYe2 --> nyS5([Subscriber 5])
        vNYe2 --> nyS4([Subscriber 4])
        vNY --> vNYe1[Edge 1]
        vNYe1 --> nyS3([Subscriber 3])
        vNYe1 --> nyS2([Subscriber 2])
        vNYe1 --> nyS1([Subscriber 1])

    end

    subgraph London/UK
        vNY ----> lV[Via Relay]
        lV --> vLe1[Edge 1]
        vLe1 --> lS3([Subscriber 3])
        vLe1 --> lS2([Subscriber 2])
        vLe1 --> lS1([Subscriber 1])

    end

    subgraph Amsterdam/NL
        lV --> aE[Edge 1]
        aE --> aS1([Subscriber 2])
        aE --> aS2([Subscriber 1])
    end

```

### Edge

Edge relays are core relays that facilitate connections from clients and all [relay types](#relay-types). The Edge
relay is a "**can do it all relay.**"

These relays often have a lower vertical scale due to them having to maintain MoQ client connections.  It 
is expected that there will be many edge relays based on MoQ client connections and bandwidth. 

**Edge relay operations:**

- Performs everything a Via relay does.
- Client connection server implementation of MoQ.
- Handle several client connections, each of which have different QUIC transport encryption/decryption with varying
  high volume of stream changes based on group and subgroups as defined in MoQ.
- Establishes data forwarding-plane to all subscribers based on subscribes and publisher announcements/connections.
- Perform authorization validation and enforcement
- Enforces administrative policy.
- Perform edge defense with client connections by implementing rate limiting and other DoS mitigations
- On-demand peering based on [selection algorithm](#selection-algorithm) to bring in Via relays to establish
  an optimized and efficient data forwarding-plane to subscribers. 
- Receives and state maintains announcements from both MoQ client publishers and Stub relays.

Edge relays are expected to scale horizontally in a stateless fashion, supporting scale up and scale down based
on demand in the region. Edge relays can be "right-sized" to fit the need of subscriber and publisher demand.

Edges relays are often placed behind network load balancers, which can support **anycast** for both client
and peering connections.

```mermaid
---
title: Edge relay behind Load Balancer (e.g., Anycast)
---
flowchart TB

    subgraph Load Balanced
        LB --> E1[Edge Relay 1]
        LB --> E2[Edge Relay 2]
        LB --> E3[Edge Relay 3]
    end

    P([Publisher]) --> LB((NLB/Anycast))
    S([Subscriber]) --> LB

```

### Stub

Stub relays are intended to be super lightweight and simple relays. They connect to one or more Edge relays only
and do not accept inbound connections. They implement MoQ server functionality to accept client connections and
to establish a data forwarding-plane for them via the relay infrastructure. A Stub relay is a **child** of
the **Edge** relay that it is connected to. 

Primary use-case for a Stub relay is to fan-out subscribers in a local area network. Example placement for Stub
relays are in residential access routers, branch offices, NAT routers, access points, floor switches, firewalls, etc. 

> [!NOTE]
> While this protocol fully supports [MoQ](https://github.com/moq-wg/moq-transport/) client connections and
> features of MoQ, it does not require publisher and subscribing clients to use MoQ. Clients can implement this
> protocol as a Stub relay. In this sense, the client is a stub relay of only one client (publisher, subscriber
> or both). 
> There are some interesting use-cases where this could be implemented on a client as a different process
> Stub relay that could efficiently optimize and aggregate multiple applications running on the same host system.
> For example, if many applications  use the Pub/Sub model and need to connect to the relay infrastructure,
> they could use this protocol as a Stub relay to aggregate those applications. 


Stub relays have the following uses, feature capabilities and restrictions:

- Must peer directly with Edge relays
- Can peer to more than one edge relay
- Will only receive subscribes based on the advertised announcements to the Edge relay
- Uses very little memory and CPU for peering
- Use-case for more than one peer is to optimize reachability to other edges
- Fully supports NAT and return traffic over the established peer connection, can be placed behind a firewall/NAT router
- Uses peer mode `Both`, control and data via the same peer connection.
- It does not connect to a cloud provider control server (not needed)
- Will not accept inbound peer connections and will not implement the full peering protocol
- The Edge relay that the Stub connects to will forward subscribes by marking itself as the source
  node. Stubs are transparent and stateless. Only the Edge relay is aware of the Stub. Other Edge and Via relays
  are unaware of Stub relays.

```mermaid
---
title: STUB to Edge
---
flowchart TD
    STUB(STUB) -- "Mode = Both" --> LB

    subgraph LB [Horizontal Scale Edge]
        nlb@{shape: subproc, label: "NLB/Anycast"} --> n@{shape: braces, label: "Pool of Edge Relays"}
        n --> E["Edge Relay (n)"]
    end
```

In the above diagram, the Stub is the **child** relay and the Edge relay is the **parent**. All subscriptions
sent by the Stub will be regenerated by the Edge relay with itself as the originator of that subscription.
In this sense, the Edge relay appears to the rest of the Relay network as the relay that has the subscriber.
**This is by design to ensure that the Stub relays, which may be in the millions, do not need to be known by
any other relay in the relay network.**

### Relay Terms

In addition to [relay types](#relay-types), relays are described using additional terms.  This section describes
the terms commonly used to describe relays. 

#### o-relay = Origin Relay

The origin relay node is always an [Edge Relay](#edge) **type**.  When a client session sends a publish
announcement (aka intent to publish), the relay becomes an origin relay. Client connections are only
accepted on Edge and Stub relay types. 

A [Stub Relay](#stub) **type** can have one or more publishers, which technically means the stub is
an origin relay, but the design of this protocol has Stub relays as a child of an Edge relay. The
Edge relay that the stub is connected to becomes the o-relay for the publisher in the data
forwarding-plane topology. 
 
#### s-relay = Subscriber Relay

The subscriber relay node is always an [Edge Relay](#edge) **type**.  When a client session subscribes,
the relay becomes the subscriber relay. Client connections are only accepted on Edge and Stub relay types. 

A [Stub Relay](#stub) **type** can have one or more subscribers, which technically means the stub is
a subscriber relay, but the design of this protocol has Stub relays as a child of an Edge relay. The
Edge relay that the stub is connected to becomes the s-relay for the subscriber in the data forwarding-plane topology. 

#### e-relay = General edge relay

An edge relay is of **type** [Edge](#edge). It implements MoQ and accepts inbound client connections as well as implement peering to act as a Via relay. Edge relays can peer with each other, with STUBs and Via relays.  An edge relay is
a do all relay. The term e-relay is a general term for an edge relay that may be an o-relay, s-relay, or acting
as a Via. 

#### v-relay = Via relay

A via relay is a relay that of **type** [Via](#via). It implements peering but not client access. A Via relay is
intended to be an aggregator (reduce bandwidth by optimized fan-out) and hairpin for traffic steering. It's lightweight
and has higher vertical scale supporting very low latency. 

#### Node



## Topologies

There are numerous topology options to form the data forwarding-plane from publisher to one or more subscribers.
The general rule of thumb is to optimize with hub-and-spoke (aka Star) peering if possible and inject Via 
relays as and where needed to aggregate efficiently to reduce congestion/bandwidth.  Injecting Via relays are used
to steer data forwarding via different IP forwarding paths to avoid and workaround problems (e.g.,
congestion, loss, high cost path, ...). They are also injected to steer data forwarding based on
administrative policy. By utilizing both Edge and Via relays, data forwarding paths can be granularly
controlled to provide highly scalable efficient low latency-loss between publisher and subscriber(s). 

### Hub-and-Spoke Topology

A hub-and-spoke (star) topology provides a simple data forwarding-plane that often results in the lowest
latency between publisher and subscribers. The design is simple; the relay that has the directly attached
publisher client session sending data (aka origin of the data) establishes peering to relays that directly have
the subscribers connected. 

The [selection algorithm](#selection-algorithm) attempts to use hub-and-spoke if all the below are true:

* administrative policy permits it
* network path has enough bandwidth and has little to no loss and acceptable latency
* publish origin relay is not overloaded with connections to other edges
* Duplication fan-out is not more than configured maximum with a default of 2

```mermaid
---
title: Hub-and-Spoke Peering Topology
---
flowchart TD    
    P([Publisher]) --> E1("Seattle Edge Relay (n)")

    E1 --> E2["Seattle Edge Relay (n+1)"]
    E1 --> E3["New York Edge Relay (n)"]


    E2 --> S1([Subscriber 1])
    E3 --> S2([Subscriber 2])
```

In the above diagram, there are two subscribers in different regions, so aggregation using a Via relay is not needed.
The seattle edge relay establishes peering to the subscriber relay and forwards data. 

### Typical Topology

The typical topology is used to support optimized data forwarding with bandwidth and network forwarding efficiency. It uses
hub-and-spoke when it make sense and adds Via(s) when needed. Via relays are a resource that is engaged on-demand or via pre-defined peering to establish hairpin/aggregation points in an optimized fashion. The Via(s) that are used are ones that are not overloaded and have optimal forwarding to the target subscriber edge relays. 
A Via is added upon [selection]](#selection-algorithm) of peering to establish the data forwarding.  Vias can be
added or removed with zero-loss in data forwarding. For example, the data forwarding-plane may start off as
hub-and-spoke and then transition to use a Via to aggregate Edges in a region. 

```mermaid
---
title: Typical Topology
---
flowchart TD    
    P([Publisher]) --> SEA

    subgraph SEA ["Seattle"]
        E1("Seattle Edge Relay (n)") --> E2["Seattle Edge Relay (n+1)"]
        E2 ---> S1([Subscriber 1])
    end

    subgraph JFK ["New York"]
        E1 --> V1["New York Via Relay (n)"]

        V1 --> E3["New York Edge Relay (n)"]
        V1 --> E4["New York Edge Relay (n+1)"]

        E3 --> S2([Subscriber 2])
        E4 --> S3([Subscriber 3])
    end
```


#### Topology Showing Alternate Paths

```mermaid
---
title: Example Typical topology showing best and alternate paths
---
flowchart TD    
    P([Publisher]) --> STUB1(Stub Relay 1)
    STUB1 --> E1["Edge Relay 1"]
    STUB1 -- BEST --> E3[Edge Relay 3]
    E1 ---> E2[Edge Relay 2]
    E1 ---> S1([Subscriber 1])
    E2 --> STUB2(STUB 2)
    STUB2 --> S2([Subscriber 2])
    E2 --> S5([Subscriber 5])

    E1 -.-> ALT_E1@{shape: braces, label: "Alternate Path 1"}
    ALT_E1 -.-> E3

    E3 --> S3([Subscriber 3])
    E3 --> S4([Subscriber 4])

    E2 -.-> ALT_E2@{shape: braces, label: "Alternate Path 2"}
    ALT_E2 -.-> E3
```



## Peering Modes

Peering connections operate in one of three modes.

### (1) Control Peering

In this mode, only control messages are exchanged. Mostly [Information Base](#information-bases) messages are exchanged
in this mode. This mode allows a single connection for control plane functions without requiring the control signaling
to follow data forwarding paths. Control signaling does not often need more than a single (can include redundant
connection for HA) while [data peering](#2-data-peering) messages can benefit from having more than one QUIC connection to scale
data forwarding.

```mermaid
---
title: Relay Control Plane
---
flowchart TD
    subgraph Relay Control-plane
        CTRL1[(Server 1)] o-.-o CTRL2[(Server 2)]
    end

    CTRL1 o-.-o E1[Edge 1]
    CTRL1 o-.-o E2[Edge 2]
    CTRL2 o-.-o E3[Edge 3]

    subgraph Data-plane
    P([Publisher]) --> STUB1(Stub 1)
    STUB1 --> E1[Edge 1]
    E1 --> E2[Edge 2]
    E1 --> E3[Edge 3]
    E1 --> S1([Subscriber 1])
    E2 --> S2([Subscriber 2])
    E3 --> S3([Subscriber 3])
    end
```

### (2) Data Peering

Data peering is used to forward data objects in a [pipeline fashion](#pipelining). Data peering can operate in
**one-way** or **two-way** modes.

In two-way mode, data peering will reuse the connection to send data back to the peer. This is to
support NAT and/or firewall constraints.

In one-way mode, the peer is used only for receiving data.

Data peering mode is indicated in the `connect` and `connect_response` messages.

```mermaid
---
title: Data Peer Multiple Connections between Relays
---
flowchart LR
    E1[Edge 1] -- "one-way 1" --> E2[Edge 2]
    E1 -- "one-way 2" --> E2
    E2 -- "one-way" --> E1
    E1 <-- "two-way" --> E3[Edge 3]
```

[Stub](#peering-modes) peers always operate in two-way mode because it is assumed that they are behind NAT.

### (3) Both control and data peering

When multiple parallel connections are used, only a single peer **MUST** be used in this mode. The peer can opperate
in one-way or two-way modes.

```mermaid
---
title: Control & Data Peer Multiple Connections between Relays
---
flowchart LR
    E1[Edge 1] -- "one-way 1 (CONTROL)" --> E2[Edge 2]
    E1 -- "one-way 2" --> E2
    E2 -- "one-way" --> E1
    E1 <-- "two-way (CONTROL)" --> E3[Edge 3]
```

Control peering is always bidirectional and uses the same peering session between two nodes.




## Information Bases

Information bases (IB) hold the control plane information. Information is advertised and withdrawn to maintain the
information bases.

### Node Information

Node information base (NIB) conveys information about a node itself. It is exchanged in `connect` and `connect_response` messages
to indicate the peer info of the nodes connecting. If the peer is a control peer, node information of other nodes are sent to the
peer. Upon transmission of the node information, the path is appended with self information. Node information is not
flooded to a peer if peer is within the node path. Upon receiving a node, if the node sees itself in the path, it is
dropped. Only best nodes selected are advertised. If it was already advertised to the same peer, it is not advertised
again, unless there is a change in the node information.

Removal of `node_info` happens when the direct peering session is terminated. Upon receiving a withdrawal of
`node_info`, the `node_info` will be removed for that peer session. The best path selection will take place to
find another path. If a new best path is found and the removal was toward the peer session that withdrew it, the
node will advertise the new best path `node_info` to all other peers. If no best path is found, it will send a withdrawal
to all other peers.

The NIB contains the following:

| Field/Attributes                | Description                                                                         |
| ------------------------------- | ----------------------------------------------------------------------------------- |
| [NodeId](#node-id)              | Node ID of the node as a unsigned 64bit integer                                     |
| **Contact**                     | FQDN or IP to reach the node. This maybe an FQDN of the load balancer or anycast IP |
| **Longitude**                   | Longitude of the node location as a double 64bit value                              |
| **latitude**                    | Latitude of the node location as a double 64bit value                               |
| [Node Relay Type](#relay-types) | Type of the node relay                                                              |
| [Node Path](#node-path)         | Path of nodes the node information has traversed                                    |

> [!NOTE]
> Other fields will be added in the future to further describe the node to support administrative policies
> and better path selections.

#### Node ID

Node ID is the unique ID of the node. Nodes **MUST** be configured with different Node IDs. If more than one Node
uses the same ID, only the first (oldest) will work. The others will be dropped.

The Node ID is an unsigned 64bit integer that is configured using the below
textual scheme. The textual scheme aids in simplified assignment, automation,
administrative configuration and troubleshooting.

**Scheme**: `<high value>:<low value>` textual format. The colon is **REQUIRED**.

The values can be represented as an unsigned 32bit integer or dotted notation of 16bit integers,
such as `<uint16>.<uint16>`. The values can be mixed in how they are represented.

**Examples**: All the below are valid configurations of the Node ID

- `1.2:1234`
- `1:1`
- `100.2:9001.2001`
- `123456:789.100`

> Simple deployments may automate the Node ID value using a 64bit hash of the node device or host ID or FQDN (if unique).

#### Node Path

Node Path is an array of node path items (NPI). NPIs are appended to the path upon advertisement via the peering
session. Self node information always is sent with an empty path. For example, the path has a length
of zero when advertising self to a peer session.

NPIs contain the following fields.

| Field               | Description                                                                      |
| ------------------- | -------------------------------------------------------------------------------- |
| [Node ID](#node-id) | Node ID of the node sending the advertisement                                    |
| **sRTT**            | Smooth round trip time of the peering session the node info was **received via** |

> [!NOTE]
> Other fields will be added in the future based on changes to [selection algorithm](#selection-algorithm)

The Node Path is used to prevent loops and to compute the best path to the Node ID being advertised. Computing the
best path also includes other node info, such as geo-distance, constraints, etc.

```mermaid
---
title: Example Node Path for Node ID 1:1
---
flowchart LR
   N1[1:1] -- "25ms, []" --> N2[1:2]
   N2 -- "50ms, [{1:2, 25ms}]" --> N3[1:3]
   N3 -- "60ms, [{1:2, 25ms}, {1:3, 50ms}]" --> N4[1:4]
   N3 -. "90ms, [{1:2, 25ms}, {1:3, 50ms}]" .-> N6

   N1 -- "20ms, []" --> N5[1:5]
   N5 -- "40ms, [{1:5, 20ms}]" --> N6[1:6]
```

Using [selection algorithm](#selection-algorithm), below shows each node and what it has computed as best path for
Node `1:1`

- Node `1:2` - Path length is `0` (direct) and sum(sRTT) is `25ms`
- Node `1:3` - Best is path length `1` and sum(sRTT) of `75ms`; alternate is path length `2` sum(sRTT) of `150ms`
- Node `1:4` - Path length is `2` and sum(sRTT) is `135ms`
- Node `1:5` - Path length is `0` (direct) and sum(sRTT) is `20ms`
- Node `1:6` - Path length is `1` and sum(sRTT) is `60ms`

If a new connection were established between `1:4` and `1:6` with a sRTT of less than `75ms`, `1:4` would
select the path via `1:6` with a path length of `2` and sum(sRTT) of less than `135ms`

### Announcement Information

MoQT announce of namespace tuple and name are advertised in `announce_info` messages. Only the hash of each namespace
item and name are sent. `announce_info` is advertised to all peers. Loop prevention is performed by not forwarding
`announce_info` messages that have already been seen.

Withdraw of `announce_info` is sent to all peers to remove an entry upon MoQT unannounce.

Announce Information (`announce_info`) contains the following:

| Field          | Description                                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------------- |
| FullNameHashes | Array of the **namespace tuple** hashes and optionally hash of **name**. Only 64bit hashes are encoded. |
| source_node_id | The node ID that received the MoQT announce                                                             |

### Subscribe Information

MoQT subscribe namespace tuple and name are advertised in `subscribe_info` messages. Only the hash of each namespace
item and name are sent. `subscribe_info` is advertised to the peering session that is the best path to reach
the source node of [matching announce](#matching-subscribes-to-announcements) info. Loop prevention is performed by not
forwarding `subscribe_info` messages that have already been seen.

Withdraw of `subscribe_info` is sent by the node that received the MoQT unsubscribe. Changes to the best source
node and peer session may change at any time, resulting subscribes being advertised via some nodes that may
not see the withdrawal when the paths have changed. To mitigate this situation, `subscribe_info` is advertised
to all peers, similar in the same fashion as [announce_info](#announcement-information).

Subscribe information (`subscribe_info`) contains the following:

| Field          | Description                                                                                 |
| -------------- | ------------------------------------------------------------------------------------------- |
| FullNameHashes | Array of the **namespace tuple** hashes and hash of **name**. Only 64bit hashes are encoded |
| source_node_id | The node ID that received the MoQT announce                                                 |

## Connection Establishment

Peers can be established by either side. Only one peer connection can be used for control peering. If multiple
parallel connections are established, the first connection made will be used for control peering bi-directionally.
Data peering can be used over the other connections. The mode of the peering session indicates control verses
data or both. If data peer, the data mode can be one-way or two-way. If two-way, return data will be sent
via the established peering connection. This is to support NAT and firewall use-cases.

A race condition exists if peer connections are initiated by both relays at the same time. In data forwarding, both
connections can be used. For control, the peer that is established first wins and if they both are established at
the same time, the peer that has the lowest Node ID wins. The result is that only one peering connection between
two Nodes will be used for control information base messages.

The control peering connection **MUST** remain established. Upon termination, all states associated to that peer
session will be removed. For example, all nodes learned/used via this session and all announces/subscribes via
this session will be removed.

## Selection Algorithm

The selection algorithm will evolve over time to include more granular metrics on load, usage, best via relays for
aggregation, and administrative/business policy constraints,...

At this time the below is implemented to select the best peering session to use.

1. Prefer the shortest path len (number of transit hops)
2. Prefer the lowest sum of sRTT for all paths and the sRTT of the peering session itself

## Data Forwarding

Data is forwarded using a data header that is included in every datagram message and start of every QUIC stream.
QUIC streams enable pipeline support where data is forwarded bytes-in to \[fan-out\] bytes-out. Pipeline forwarding
reduces the end-to-end latency and jitter on data between publisher edge relay to all subscriber edge/stub relays.

### Source Routing

Data forwarding is source routed in a similar fashion as described in [Segment Routing Architecture](https://www.rfc-editor.org/rfc/rfc8402.html).
Data forwarding in MoQT is designed to support fan-out of one or more publishers to one or more subscribers. This
is different from IP forwarding (point to point) in terms of the stack size of labels. Building a stack of labels
for all target nodes could grow into the thousands with a large number of subscribers spanning thousands
of edge relays. This would not scale to send the set of node IDs in every datagram frame or even start of every QUIC
stream, considering QUIC streams may change often due to group/subgroup changes.

Unlike segment routing where it utilizes a stack of labels/sids, this protocol utilizes
a **subscriber node set (SNS)** that **describes the set of subscriber edge relay node IDs that
need to receive the data**. It does **not describe** the path that the data will traverse.

SNS is exchanged via the peer session that will receive the data using the control stream via that
same peer session. Utilizing the control bidir stream within the peer session ensures scope of the SNS
to be within the peer session to support parallel peer sessions and control information based peering
that is not using the same data path. Each SNS will be assigned an SNS ID that is unique to the session.
The sender generates the SNS ID.

The SNS ID is an `unsigned 32bit` integer that is a monotonic series increasing by one.
The ID starts at ONE and can wrap to ONE as needed. Zero is reserved to indicate no ID.

Withdraws remove SNS IDs from active state. States are peer scoped and will be removed upon peer session close.

The design of subscriber node sets is to be fast and lightweight with little control signaling involvement to maintain
state. Node sets can change often based on node (e.g., relay) churn with peering sessions and subscribers. This
will result in the SNS being updated. Using a new ID to replace the previous introduces race conditions where data
could be lost. Instead of replacing the ID, the same ID is updated with a new set to allow a smooth transition
without invalidating a previous set id.

```mermaid
---
title: Source Routing Data Forwarding
---
flowchart TD
    P([Publisher]) -- MoQT Data Object --> OE[Origin 101.2:10]
    OE -- " SNSid: 1=[100.2:1, 100.2:2, 103.2:1] " --> V1[Via 100.1:1]

    subgraph US-WEST
        V1 -- " SNSid: 18=[100.2:1] " --> w1[Edge 100.2:1]
        w1 -- MoQT Data Object --> wS1([Subscriber 1])
        w1 -- MoQT Data Object --> wS2([Subscriber 2])
        V1 -- " SNSid: 27=[100.2:2] " --> w2[Edge 100.2:2]
        w2 -- MoQT Data Object --> wS3([Subscriber 3])
        w2 -- MoQT Data Object --> wS4([Subscriber 4])
    end
    subgraph US-EAST
        V1 -- " SNSid: 99=[103.2:1] " --> e1[Edge 103.2:1]
        e1 -- MoQT Data Object --> eS3([Subscriber 1])
        e1 -- MoQT Data Object --> eS4([Subscriber 2])

    end
```

#### SNS Advertisement

A new transport data context is created to handle the track data flow. Considering the transport
connection ID already provides a session specific ID that increments by one, it can be used
as the SNS ID. In order to create a new data context, the session needs to be known. Therefore,
a SNS is created with a computed set of downstream nodes by peering session that should be used,
which is based on [selection](#selection-algorithm). Upon identifying the peer session with
the SNS per peer, a data context is created that provides the ID. The SNS is then advertised
via the control bidir stream in parallel with data being transmitted and encoded with
the [data header](#start-of-data-header).

Considering the receiving side may receive data with an SNS ID that it doesn't know about yet,
the receiving side will buffer the messages based on a negotiated buffer time period. If
the SNS ID is not received by that time, the data would be dropped in a circular buffer fashion.

SNS information message has the following fields:

| Field           | Description                                                               |
| --------------- | ------------------------------------------------------------------------- |
| SNS ID          | SNS ID of this entry                                                      |
| Target Node Set | Target node ID set. Array of all nodes that should be forwarded this data |

The target node set is generated based on the [Best Node Information](#node-information) via the peering session.
For example, when subscribes are received by the publisher relay (aka origin relay) the subscriber information
will indicate source nodes of where that subscribe originated. This node id will be added to a set that
is relative to the peering session that reaches that node based on the [selection algorithm](#selection-algorithm)
This will result in different sets based on selection of which peering session reaches best the edge nodes that need
to receive the data. In large scale, multiple data peering sessions will be utilized, so the sets will be
a subset of total number of subscriber edge nodes. STUBS are not included. Only Edge relays are added to
this set.

#### SNS Withdrawal

Withdraw SNS information message has the following fields:

| Field  | Description                   |
| ------ | ----------------------------- |
| SNS ID | SNS ID of the entry to remove |

### Start of Data Header

| Field  | Description                                                                                                                          |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| SNS ID | unsigned 32bit unique subscriber node set ID within the session scope that identifies which target nodes the data should be sent to. |

> [!NOTE]
> TODO will add more fields as needed

The SNS ID is changed hop by hop, but it uses a fixed 32 bit value supporting fast offset based changing of the value
with datagram messages. QUIC streams only have this header on start of QUIC stream. Only on new QUIC stream is the
start of data header included.

### Supporting MoQT data Objects in pipeline forwarding

A complication with pipeline forwarding is that the start and end of an object are not known by the intermediate
nodes when relaying the data via QUIC streams. Datagram is moot as each datagram frame is the complete object.
Only the receiving edge relay needs to know the start/end of MoQT QUIC streamed data objects so the data be cached and
sent correctly to the subscriber(s). To support this with QUIC streams, an additional header for each object is
used (QUIC streams only). All MoQT data via QUIC streams is sent using the below header to wrap the data object.

| Field                | Description                                                                           |
| -------------------- | ------------------------------------------------------------------------------------- |
| track full name hash | Track full name hash (aka MoQ track alias)                                            |
| payload length       | QUIC variable length integer length value that indicates the payload length to follow |
| payload              | Payload bytes based on the length indicated                                           |

### Matching Subscribes to Announcements

Subscribes are delivered via the control information base forwarding to the publisher edge relay. Matching
the announce to subscribe is performed by matching the namespace tuple of hashes and name in order. If a match is made,
the subscribe is sent via the peering session that would be best to reach the source node of the publisher.

### Optimizing Peering using MoQT GOAWAY

In large scale deployments, the usage of [source routing data forwarding](#source-routing) can result in large sets
of target edge nodes that have subscribers to receive data. This can be suboptimal if every subscriber was on
a different edge relay, especially when in the same region. This could happen due to network load balances,
including anycast, where subscriber clients are distributed to one of hundreds of relays within the same region.
It is desirable to align subscribers of the same content to use the same relays, providing load/capacity is available.
MoQT provides a mechanism to redirect a client connection to another relay. This method is to use a GOWAY with
a new connect URL. This protocol uses the GOAWAY (aka redirect) to redirect client connections to a nearby relay that
has the same subscriptions.

Edge relays are stateless, but they do have the control information bases to look this up received
subscription information from other relays and node information to intelligently balance/move clients to
other relays that have the same subscribes.

## Messages Formats
Peering transmits wire format messages.  This section defines all the peering messages and their encoded
wire format. 

### Connect Message

### Connect Response Message

### Data Object Message

### Node Information Advertisement Message

### Node Information Withdraw Message

### Subscribe Information Advertisement Message

### Subscribe Information Withdraw Message

### Announce Information Advertisement Message

### Announce Information Withdraw Message

### Subscribe Node Set Advertisement Message

### Subscribe Node Set Withdraw Message


## Message Flows

Various message flows

### Connection Establishment

```mermaid
---
title: Connection Establishment
---
sequenceDiagram
    actor R as Relay
    participant PM as Relay: Peer Mgr
    participant PS as Relay: Peer Session
    R ->> PM: Connect (self node info<br/> w/ params)
    PM -->> PS: Create Session
    PS -->> PS: Add Node to NIB
    PS ->> R: ConnectResponse(this node info w/ params)
    PS -->> PS: Compute Best Path
    R -) PS: Create bidir Control Stream
    PS ->> PS: Latch bidir stream as control stream

    Note right of R: Using control peering mode

    loop If not STUB
        R -)+ PS: [node_info, ...]
        PS -)+ R: [node_info, ...]
        PS -)- R: [announce_info, ...]
    end

    R -) PS: [announce_info, ...]

    loop Subscribe Info<br/>if best based on announce
        R -)+ PS: [subscribe_info, ...]
        PS -)- R: [subscribe_info, ...]
    end

```

### Publish Announcement

Announces are flooded to all control peers, except to STUB peers. STUB peers do not require announces as they use
static routing. By default, STUB peers will advertise all subscribes.

```mermaid
---
title: Publish Announcement
---
sequenceDiagram
    actor P as Publisher
    participant CS as Relay: Client Server
    participant PM as Relay: Peer Mgr
    participant PS as Relay: Peer Session
    actor R as Control/Other Relay
    P ->> CS: ANNOUNCE

CS -->> PM: ClientAnnounce

    loop All Peers except
        PM -->> PS: AdvertiseAnnounce
        Note left of PS: May batch announces
        PS ->> R: AdvertiseAnnounce
    end
```

### Subscribe Request

Subscribes are only sent to best peer sessions based on announce source node.

```mermaid
---
title: Subscriber Requests
---
sequenceDiagram
    actor P as Subscribe
    participant CS as Relay: Client Server
    participant PM as Relay: Peer Mgr
    participant PS as Relay: Peer Session
    actor R as Control/Other Relay
    P ->> CS: SUBSCRIBE
    CS -->> PM: ClientSubscribe
    Note right of PM: Find announce nodes

    loop BEST Announce Peer
        PM -->> PS: AdvertiseSubscribe
        Note left of PS: May batch subscribes
        PS ->> R: AdvertiseSubscribe
    end
```
