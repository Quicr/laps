# LAPS Peering Protocol and Architecture

LAPS peering architecture enables peering between relays and clients, supporting granular traffic engineering and optimizations. Peering can be simple mesh style, or it can be highly scalable for large infrastructure by using on-demand Via peering sessions to aggregate traffic flows in the most efficient way to provide low loss/latency fan-out delivery.

Peering sessions support administrative policy constraints to ensure that traffic is forwarded via a traffic-engineered path using one or more relays in the network. Traffic-engineered paths can be human or AI-defined to provide low duplication and even load distribution while maintaining administrative policies. For example, AI can optimize relay forwarding to support different classes of service (e.g., gold, silver, bronze) while also supporting geographic region constraints. IP forwarding paths are a factor in maintaining administrative policies. For example, if forwarding between relay A in New York to relay B in Seattle would IP packet forward via Canada, AI would find another relay to ensure IP forwarding path compliance with the administrative policy. 

Peering scales to a global network of relays supporting hundreds of millions of active tracks with any combination of publishers and subscribers.

- [LAPS Peering Protocol and Architecture](#laps-peering-protocol-and-architecture)
  - [Relay Types](#relay-types)
    - [Via](#via)
    - [Edge](#edge)
    - [Stub](#stub)
    - [Relay Terms](#relay-terms)
      - [o-relay = Origin Relay](#o-relay--origin-relay)
      - [s-relay = Subscriber Relay](#s-relay--subscriber-relay)
      - [e-relay = General edge relay](#e-relay--general-edge-relay)
      - [v-relay = Via relay](#v-relay--via-relay)
      - [Node](#node)
  - [Topologies](#topologies)
    - [Hub-and-Spoke Topology](#hub-and-spoke-topology)
    - [Typical Topology](#typical-topology)
      - [Topology Showing Alternate Paths](#topology-showing-alternate-paths)
  - [Peering Modes](#peering-modes)
    - [(1) Control Peering](#1-control-peering)
      - [Sending/Receiving Control Messages](#sendingreceiving-control-messages)
    - [(2) Data Peering](#2-data-peering)
    - [(3) Both control and data peering](#3-both-control-and-data-peering)
  - [Information Bases](#information-bases)
    - [Node Information](#node-information)
      - [Node ID](#node-id)
      - [Node Path](#node-path)
    - [Announcement Information](#announcement-information)
    - [Subscribe Information](#subscribe-information)
  - [MoQT Track and Relay Peer Handling](#moqt-track-and-relay-peer-handling)
  - [Connection Establishment](#connection-establishment)
  - [Selection Algorithm](#selection-algorithm)
  - [Data Forwarding](#data-forwarding)
    - [Source Routing](#source-routing)
      - [SNS Advertisement](#sns-advertisement)
        - [FIB](#fib)
      - [SNS Withdrawal](#sns-withdrawal)
    - [Pipeline forwarding](#pipeline-forwarding)
    - [Matching Subscribes to Announcements](#matching-subscribes-to-announcements)
  - [Messages](#messages)
    - [Errors](#errors)
      - [QUIC APP Error Codes](#quic-app-error-codes)
      - [Protocol Response Codes](#protocol-response-codes)
    - [Control Messages](#control-messages)
      - [Common Control Headers](#common-control-headers)
        - [Common Header Message Types](#common-header-message-types)
      - [Connect Message](#connect-message)
      - [Connect Response Message](#connect-response-message)
      - [Node Information Advertisement Message](#node-information-advertisement-message)
      - [Node Information Withdraw Message](#node-information-withdraw-message)
      - [Subscribe Information Advertisement Message](#subscribe-information-advertisement-message)
      - [Subscribe Information Withdraw Message](#subscribe-information-withdraw-message)
      - [Announce Information Advertisement Message](#announce-information-advertisement-message)
      - [Announce Information Withdraw Message](#announce-information-withdraw-message)
      - [Subscribe Node Set Advertisement Message](#subscribe-node-set-advertisement-message)
      - [Subscribe Node Set Withdraw Message](#subscribe-node-set-withdraw-message)
    - [Data Object Messages](#data-object-messages)
        - [Data Object Common Header](#data-object-common-header)
          - [Data Object Types](#data-object-types)
      - [Datagram Objects](#datagram-objects)
      - [Start of New Stream Data Object](#start-of-new-stream-data-object)
      - [Subsequent Stream Data Objects](#subsequent-stream-data-objects)
  - [Considerations](#considerations)
    - [Optimizing Peering using MoQT GOAWAY](#optimizing-peering-using-moqt-goaway)
    - [Unsubscribe/Subscribe Misuse](#unsubscribesubscribe-misuse)
  - [Message Flows](#message-flows)
    - [Connection Establishment](#connection-establishment-1)
    - [Node Advertisement](#node-advertisement)
    - [Subscribe Advertisement](#subscribe-advertisement)
    - [TODO Implementation](#todo-implementation)

## Relay Types

The relay type is configured upon startup of the relay process. A relay can change its type at runtime, but it must disconnect all peers and reconnect them to synchronize the type. The relay type is advertised in [node information](#node-information) upon connection establishment. 

Not all relays need to perform the same functions. There are also specific use cases for relays based on their position in the topology and how they are used. 


Relays are therefore categorized into the following types:

### Via

A Via relay is used by Edge relays to forward data between other Edge relays. A Via relay is an intermediate relay that does not participate in all the [Information Base](#information-bases) exchanges. Specifically, it only needs to participate in node information base exchanges. 

These relays have high vertical scale due to the lower number of QUIC connections between other Edge/Via relays and because of [source routing](#source-routing) and no MoQT client connections. 

> [!NOTE]
> A Via is not intended to be burdened by the global state of subscriptions, but a Via can be configured to reflect [subscribe information](#subscribe-information) when [control peering](#1-control-peering) servers are not used or available. The Via in this case merely stores and propagates subscriptions. 

**Via relay operations:**

- Does not need to be part of the `announce` and `subscribe` advertisements and withdrawals selection for data plane forwarding
- Participates in the `node` advertisements and withdrawals to maintain reachability information to all nodes
- `announce` and `subscribe` advertisements/withdrawals are state-maintained, but only to prevent looping and duplication. The retention for this state is set to the maximum convergence time. In this sense, it is a cache of seen announces and subscribes in the last several seconds
- Forwards data streams and datagram objects based on [source routing](#source-routing) using the node information base
- Traditionally has high vertical scale to support a large number of tracks and bandwidth
- Strategically placed geographically near Edge relays to provide an aggregation point for fan-out of many Edge relays
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

Edge relays are core relays that facilitate connections from clients and all [relay types](#relay-types). The Edge relay is a "**can do it all relay**."

These relays often have a lower vertical scale due to them having to maintain MoQT client connections. It is expected that there will be many Edge relays based on MoQT client connections and bandwidth. 

**Edge relay operations:**

- Performs everything a Via relay does
- Client connection server implementation of MoQT
- Handles several client connections, each of which have different QUIC transport encryption/decryption with varying high volume of stream changes based on group and subgroups as defined in MoQT
- Establishes data forwarding plane to all subscribers based on subscribes and publisher announcements/connections
- Performs authorization validation and enforcement
- Enforces administrative policy
- Performs edge defense with client connections by implementing rate limiting and other DoS mitigations
- On-demand peering based on [selection algorithm](#selection-algorithm) to bring in Via relays to establish an optimized and efficient data forwarding plane to subscribers 
- Receives and state-maintains announcements from both MoQT client publishers and Stub relays

Edge relays are expected to scale horizontally in a stateless fashion, supporting scale-up and scale-down based on demand in the region. Edge relays can be "right-sized" to fit the need of subscriber and publisher demand.

Edge relays are often placed behind network load balancers, which can support **anycast** for both client and peering connections.

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

Stub relays are intended to be super lightweight and simple relays. They connect to one or more Edge relays only and do not accept inbound connections. They implement MoQT server functionality to accept client connections and to establish a data forwarding plane for them via the relay infrastructure. A Stub relay is a **child** of the **Edge** relay that it is connected to. 

Primary use case for a Stub relay is to fan-out subscribers in a local area network. Example placement for Stub relays are in residential access routers, branch offices, NAT routers, access points, floor switches, firewalls, etc. 

> [!NOTE]
> While this protocol fully supports [MoQT](https://github.com/moq-wg/moq-transport/) client connections and features of MoQT, it does not require publisher and subscribing clients to use MoQT. Clients can implement this protocol as a Stub relay. In this sense, the client is a stub relay of only one client (publisher, subscriber, or both). 
> There are some interesting use cases where this could be implemented on a client as a different process Stub relay that could efficiently optimize and aggregate multiple applications running on the same host system. For example, if many applications use the Pub/Sub model and need to connect to the relay infrastructure, they could use this protocol as a Stub relay to aggregate those applications. 


Stub relays have the following uses, feature capabilities and restrictions:

- Must peer directly with Edge relays
- Can peer to more than one Edge relay
- Will only receive subscribes based on the advertised announcements to the Edge relay
- Uses very little memory and CPU for peering
- Use case for more than one peer is to optimize reachability to other edges
- Fully supports NAT and return traffic over the established peer connection, can be placed behind a firewall/NAT router
- Uses peer mode `Both`, control and data via the same peer connection
- Does not connect to a cloud provider control server (not needed)
- Will not accept inbound peer connections and will not implement the full peering protocol
- The Edge relay that the Stub connects to will forward subscribes by marking itself as the source node. Stubs are transparent and stateless. Only the Edge relay is aware of the Stub. Other Edge and Via relays are unaware of Stub relays

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

In the above diagram, the Stub is the **child** relay and the Edge relay is the **parent**. All subscriptions sent by the Stub will be regenerated by the Edge relay with itself as the originator of that subscription. In this sense, the Edge relay appears to the rest of the relay network as the relay that has the subscriber. **This is by design to ensure that the Stub relays, which may be in the millions, do not need to be known by any other relay in the relay network.**

### Relay Terms

In addition to [relay types](#relay-types), relays are described using additional terms. This section describes the terms commonly used to describe relays. 

#### o-relay = Origin Relay

The origin relay node is always an [Edge Relay](#edge) **type**. When a client session sends a publish announcement (aka intent to publish), the relay becomes an origin relay. Client connections are only accepted on Edge and Stub relay types. 

A [Stub Relay](#stub) **type** can have one or more publishers, which technically means the stub is an origin relay, but the design of this protocol has Stub relays as a child of an Edge relay. The Edge relay that the stub is connected to becomes the o-relay for the publisher in the data forwarding plane topology. 
 
#### s-relay = Subscriber Relay

The subscriber relay node is always an [Edge Relay](#edge) **type**. When a client session subscribes, the relay becomes the subscriber relay. Client connections are only accepted on Edge and Stub relay types. 

A [Stub Relay](#stub) **type** can have one or more subscribers, which technically means the stub is a subscriber relay, but the design of this protocol has Stub relays as a child of an Edge relay. The Edge relay that the stub is connected to becomes the s-relay for the subscriber in the data forwarding plane topology. 

#### e-relay = General edge relay

An edge relay is of **type** [Edge](#edge). It implements MoQT and accepts inbound client connections as well as implements peering to act as a Via relay. Edge relays can peer with each other, with STUBs and Via relays. An edge relay is a do-all relay. The term e-relay is a general term for an edge relay that may be an o-relay, s-relay, or acting as a Via. 

#### v-relay = Via relay

A via relay is a relay of **type** [Via](#via). It implements peering but not client access. A Via relay is intended to be an aggregator (reduce bandwidth by optimized fan-out) and hairpin for traffic steering. It's lightweight and has higher vertical scale supporting very low latency. 

#### Node

Relay could be implemented in a client, such as a Stub relay in the client. In this case, the client relay would not be called a node. Node term is used to clarify that the relay of any type is an infrastructure-level relay that participates in peering. 


## Topologies

There are numerous topology options to form the data forwarding plane from publisher to one or more subscribers. The general rule of thumb is to optimize with hub-and-spoke (aka Star) peering if possible and inject Via relays as and where needed to aggregate efficiently to reduce congestion/bandwidth. Injecting Via relays are used to steer data forwarding via different IP forwarding paths to avoid and work around problems (e.g., congestion, loss, high cost path, etc.). They are also injected to steer data forwarding based on administrative policy. By utilizing both Edge and Via relays, data forwarding paths can be granularly controlled to provide highly scalable, efficient, low latency-loss between publisher and subscriber(s). 

### Hub-and-Spoke Topology

A hub-and-spoke (star) topology provides a simple data forwarding plane that often results in the lowest latency between publisher and subscribers. The design is simple; the relay that has the directly attached publisher client session sending data (aka origin of the data) establishes peering to relays that directly have the subscribers connected. 

The [selection algorithm](#selection-algorithm) attempts to use hub-and-spoke if all the below are true:

- Administrative policy permits it
- Network path has enough bandwidth and has little to no loss and acceptable latency
- Publish origin relay is not overloaded with connections to other edges
- Duplication fan-out is not more than configured maximum with a default of 2

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

In the above diagram, there are two subscribers in different regions, so aggregation using a Via relay is not needed. The Seattle origin relay establishes peering to the subscriber relay and forwards data. 

### Typical Topology

The typical topology is used to support optimized data forwarding with bandwidth and network forwarding efficiency. It uses hub-and-spoke when it makes sense and adds Via(s) when needed. Via relays are a resource that is engaged on-demand or via predefined peering to establish hairpin/aggregation points in an optimized fashion. The Via(s) that are used are ones that are not overloaded and have optimal forwarding to the target subscriber edge relays. A Via is added upon [selection](#selection-algorithm) of peering to establish the data forwarding. Vias can be added or removed with zero-loss in data forwarding. For example, the data forwarding plane may start off as hub-and-spoke and then transition to use a Via to aggregate e-relays in a region. 

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

In this mode, only control messages are exchanged. Mostly [Information Base](#information-bases) messages are exchanged in this mode. This mode allows a single connection for control plane functions without requiring the control signaling to follow data forwarding paths. Control signaling does not often need more than a single connection (can include redundant connection for HA) while [data peering](#2-data-peering) messages can benefit from having more than one QUIC connection to scale data forwarding over network ECMP paths. 

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

#### Sending/Receiving Control Messages

Control messages are exchanged over a QUIC bi-directional stream. The peer that makes the connection is required to create the bi-directional stream that will be used for control messaging. All control messages will go over this QUIC stream. [Connect](#connect-message) is required to be the first message exchanged on a new connection. To exchange this message, the client **MUST** create the control bi-directional QUIC stream. The server side will latch onto this stream to send back responses and subsequent control messages to the client. The latching allows the client to recreate the control bi-directional stream if needed. The server will latch and move to the new stream based on received control messages from the client. At no point can two bi-directional control streams be in use. To ensure this, the server will reset the previous control bidir stream if it wasn't already reset/fin closed. 

### (2) Data Peering

Data peering is used to forward data objects in a [pipeline fashion](#pipeline-forwarding). Data peering can operate in **uni-directional** or **bi-directional** modes.

In bi-directional mode, data peering will reuse the connection to send data back to the peer. This is to support NAT and/or firewall constraints.

In uni-directional mode, the peer is used only for receiving data.

Data peering mode is indicated in the `connect` and `connect_response` messages.

> [!IMPORTANT]
> In data peering mode only, the control bidirectional stream is used to convey [SNS](#data-forwarding) advertisements. In this sense, the control stream is always established, even when control peering is not used for the data peer. 

```mermaid
---
title: Data Peer Multiple Connections between Relays
---
flowchart LR
    E1[Edge 1] -- "unidir 1" --> E2[Edge 2]
    E1 -- "unidir 2" --> E2
    E2 -- "unidir" --> E1
    E1 <-- "bidir" --> E3[Edge 3]
```

[Stub](#stub) peers always operate in bi-directional mode because it is assumed that they are behind NAT/FW. Stub relays also do not have the scale that requires multiple connections to support network ECMP. 

### (3) Both control and data peering

When multiple parallel connections are used, only a single peer **MUST** be used in this mode. The peer can operate in uni-directional or bi-directional modes. 

```mermaid
---
title: Control & Data Peer Multiple Connections between Relays
---
flowchart LR
    E1[Edge 1] -- "one-way 1 (CONTROL)" --> E2[Edge 2]
    E1 -- "unidir 2" --> E2
    E2 -- "unidir" --> E1
    E1 <-- "bidir (CONTROL)" --> E3[Edge 3]
```

Control peering is always bi-directional and uses the same peering session. 

## Information Bases

Information bases (IB) hold the control plane information. Information is advertised and withdrawn to maintain the information bases.

### Node Information

Node information base (NIB) conveys information about a node itself. It is exchanged in [connect](#connect-message) and [connect_response](#connect-response-message) messages to indicate the peer info of the nodes connecting. If the peer is a control peer, node information of other nodes are sent to the peer. Upon transmission of the node information, the path is appended with self node ID. This forms a node path so that forwarding can stop when it sees remote or itself in the path list. Only best nodes selected are advertised. Node information is not sent if it's the same as received before (duplicate). It will be advertised if there is a change. This is to support metric changes, such as load and reachability information changing in node information.

Currently a version of the node update is not required, but may be added in the future if race conditions arise. 

Removal of `node_info` is performed when the direct peering session is terminated or upon receiving a withdraw of the node information. The node withdraw will be sent to all peers except the one that sent it and if the peer is in the path. 

The best path selection will take place to find another path for active subscribes that are using the node that is withdrawn. 

The NIB contains the following:

| Field/Attributes          | Description                                                                          |
| ------------------------- | ------------------------------------------------------------------------------------ |
| [NodeId](#node-id)        | Node ID of the node as an unsigned 64-bit integer                                    |
| [Node Type](#relay-types) | Node Relay Type                                                                      |
| **Contact**               | FQDN or IP to reach the node. This may be an FQDN of the load balancer or anycast IP  |
| **Longitude**             | Longitude of the node location as a double 64-bit value                               |
| **Latitude**              | Latitude of the node location as a double 64-bit value                                |
| [Node Path](#node-path)   | Path of nodes the node information has traversed                                     |
| SumSrtt                   | Sum of SRTT in microseconds for peering sessions in the path, zero if peer is direct |

> [!NOTE]
> Other fields will be added in the future to further describe the node to support administrative policies
> and better path selections.

#### Node ID

Node ID is the unique ID of the node. Nodes **MUST** be configured with different Node IDs. If more than one Node
uses the same ID, only the first (oldest) will work. The others will be dropped.

The Node ID is an unsigned 64-bit integer that is configured using the below
textual scheme. The textual scheme aids in simplified assignment, automation,
administrative configuration and troubleshooting.

**Scheme**: `<high value>:<low value>` textual format. The colon is **REQUIRED**.

The values can be represented as an unsigned 32-bit integer or dotted notation of 16-bit integers,
such as `<uint16>.<uint16>`. The values can be mixed in how they are represented.

**Examples**: All the below are valid configurations of the Node ID

- `1.2:1234`
- `1:1`
- `100.2:9001.2001`
- `123456:789.100`

> Simple deployments may automate the Node ID value using a 64-bit hash of the node device or host ID or FQDN (if unique).

#### Node Path

Node Path is an array of node path items (NPI). NPIs are appended to the path upon advertisement via the peering
session. Self node information always is sent with an empty path. For example, the path has a length
of zero when advertising self to a peer session.

NPIs contain the following fields.

| Field               | Description                                                                                      |
| ------------------- | ------------------------------------------------------------------------------------------------ |
| [Node ID](#node-id) | Node ID of the node sending the advertisement                                                    |
| **sRTT**            | Smooth round trip time in microseconds of the peering session the node info was **received via** |

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
item and name are advertised. `announce_info` is advertised to all peers **from Stub relays only**. Announce information
is not used by the other relays because they have all subscribe information. Stubs do not have all subscribes, so
the o-relay needs the announce info so that it can send only the subscribes matching the announces to the stub.

Loop prevention is performed by not forwarding `announce_info` messages that have already been seen. 

Withdraw of `announce_info` is sent to all peers to remove an entry upon MoQT unannounce.

Announce Information (`announce_info`) contains the following:

| Field          | Description                                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------------- |
| FullNameHashes | Array of the **namespace tuple** hashes and optionally hash of **name**. Only 64-bit hashes are encoded. |
| source_node_id | The node ID that received the MoQT announce                                                             |

### Subscribe Information

MoQT subscribe namespace tuple and name are advertised in `subscribe_info` messages. The hashes of namespace tuple
and name are the primary information being used by the relays for peering.  The hashes combined establish a unique subscribe. 

Subscribe information in MoQT includes more information, such as parameters, priority, etc. These
values, including the original opaque data of the subscribe namespace and name, are needed by o-relays to
interact with the MoQT publisher. In order to ensure that nothing is left out, the original subscribe data (entire subscribe) message is encoded in the `subscribe_info`. This allows any relay or control server to inspect and process
the original subscribe message. The original subscribe data is not used by other non o-relays and is instead
transparently passed along. 

`subscribe_info` is advertised to all control peering peers. As described in
[Control Peering](#1-control-peering), this may be forwarded in a highly scalable decentralized control plane
for peering information bases.  The control plane is not the data forwarding plane.  It's only for control and
information base messages, which will mostly be subscribe information.  

Subscribe information is designed to be scoped by administrative policy controls. This is similar to BGP
route policies. The lack of subscribe info is a form of control and filtering that directly impacts data forwarding
to subscribers.  For example, subscribes outside of the allowed region for a publisher will never be seen
by the publisher o-relay... resulting in those out of region subscribers not receiving the content. Out of
region subscribers in this case could quickly be notified of being out of region based on the administrative
policy controls. 

Subscribe to publisher is a function of [matching announce](#matching-subscribes-to-announcements) info. 

Loop prevention is performed by not forwarding `subscribe_info` messages that have already been seen and by
not sending the subscribe back to the one that originated it. 

Withdraw of `subscribe_info` is sent by the node that received the MoQT unsubscribe. Withdraws are sent
in the same fashion as advertisements to control peers. 

> [!NOTE]
> There are race conditions introduced with propagation between control peering peers. If there was enough
time between withdraw and advertisement to propagate and sync state before changing from withdraw to advertised,
then there would be no issues. On the other hand, if a withdraw happens very fast (e.g., sub-second) followed by an
advertisement it can result in a withdraw being received after an advertisement that should have been the
actual final state.  To mitigate this issue, a `sequence` number is added to the `subscribe_info` message.
The sequence number increases for every s-relay message sent. Control peering relays in the middle do not
increment the sequence number. This allows any relay to detect the proper order of withdraw to advertisement
so the final state will correctly be established. 

Subscribe information (`subscribe_info`) contains the following:

| Field          | Description                                                                                 |
| -------------- | ------------------------------------------------------------------------------------------- |
| sequence       | Sequence number to indicate the subscribe advertisement/withdraw message, set by s-relay    |
| source_node_id | The node ID that received the MoQT subscribe                                                |
| TrackHash      | Array of the **namespace tuple** hashes and hash of **name**. Only 64-bit hashes are encoded |
| subscribe_data | Original MoQT subscribe message (wire format) that initiated the subscribe                  |

## MoQT Track and Relay Peer Handling

Track as defined by MoQT is a data flow that is identified by track alias, which is a unique value that 
represents the full track name (namespace tuple and name) in subscribes. Fan-out is made possible by relaying
the data from a given track alias to all subscribes (e.g., s-relay, subscribers) that match the same
track alias. 

```mermaid
---
title: MoQT Fan-out
---
flowchart TD
    P(Publisher) --> TA@{ shape: bow-rect, label: "Track Alias = **ABC**" }
    style TA fill:#FCF3CF,stroke:#424949,stroke-width:2px,color:#1C2833,stroke-dasharray: 5 5
    TA --> S1[Subscriber 1]
    TA --> S2[Subscriber 2]
    TA --> S3[Subscriber 3]
    TA --> S4[Subscriber 4]
```

In the above high level diagram, it illustrates that publisher publishes to a track alias. That track alias matches
a set of subscribers using the same alias. Publisher **MUST** publish to the exact subscribe track alias. 
Data is then fanned-out to all the matching subscribers. 

> [!IMPORTANT]
> In MoQT the track alias can be different between subscribers and publishers.  The peering architecture normalizes track alias to be a consistent hash of the opaque namespace tuple and name.  Subscribers and publishers can still use their own track alias, but those will be mapped to the consistent hash upon forwarding via peering. For efficiency, it's recommended to use the `SUBSCRIBE_ERROR(retry track alias)` method to have both publisher and subscribers use the same consistent hash algorithm for track alias.

In peering, the track is referred to as a **data context** using QUIC layer transmission. In MoQT,
the data context uniquely identifies a flow of same content data (e.g., track, file, ...) that can
span over many streams. As with MoQT, there can only be one active QUIC stream at a time for the same
data context (e.g., track). Transitioning to a new QUIC stream primarily is to mitigate some problem with
the stream of data, such as head of line blocking. It can also be used by an application to restart
a content stream at a new point. For this reason, a new stream results in a replacement operation of the
previously active QUIC stream. Data relating to the previous QUIC stream is cleared and upon new stream data flows
afresh. This is required for [pipeline forwarding](#pipeline-forwarding) so that data does not become corrupted. 

A **data context** has an `Id` that is provided by the transport, that is connection (peer and MoQT client) session
specific.  [Subscriber Node Set Id](#source-routing) is created for each data context, which is for each
MoQT track.  An implementation may use the same `Id` for both SNS and data context ID.  

> [!NOTE]
> The reason why data context Id is not directly using track alias value is because connections
> are ephemeral to support encoding in 4 bytes maximum instead of up to 8 (wrapping supported) at the
> session level while providing a guarantee of ZERO collisions. Data context is agnostic to
> the data that is being sent. More than one data context Id could map to the same track alias. This is 
> to support multiple publishers, where publishers are publihsing to the same track alias. Data is
> [pipline forwarded](#pipeline-forwarding) and requires a single publisher for the pipe (aka data context).

In MoQT, data objects contain a **group-id**, **subgroup-id**, and **object-id**.  When the 
**group-id** or **subgroup-id** changes, the MoQT behavior is to transition to a new QUIC stream. MoQT
in this case adds more state and comparison tracking that isn't needed needed for relay forwarding,
especially with peering. The publisher will be the one that detects group/subgroup id changes and will
start a new QUIC stream. The receiving relay will efficiently see that a new stream is being
used for a data context (e.g., track). Upon this, the relay will relay that change to all
peers and clients. In this sense, the relay and peering follow the received QUIC stream transitions. 

```mermaid
---
title: Relay Peering Fan-out
---
flowchart TD
    P(Publisher) --> TA1@{ shape: bow-rect, label: "Track Alias = **ABC**" }
    style TA1 fill:#FCF3CF,stroke:#424949,stroke-width:2px,color:#1C2833,stroke-dasharray: 5 5

    P(Publisher) --> TA2@{ shape: bow-rect, label: "Track Alias = **XYZ**" }
    style TA2 fill:#FCF3CF,stroke:#424949,stroke-width:2px,color:#1C2833,stroke-dasharray: 5 5

    TA1 --> OR[o-relay]    
    TA2 --> OR[o-relay]    

    OR -- "Data CTX: A = ABC" --> SR[s-relay]
    OR -- "Data CTX: B = XYZ" --> SR
    
    SR -- "Data CTX: 1 = ABC" --> S1[Subscriber 1]
    SR -- "Data CTX: 2 = XYZ" --> S2[Subscriber 2]
```

> [!IMPORTANT]
> In the above diagram there is a single publisher, but there could be more than one publisher that is
publishing to the same track alias. While MoQT doesn't yet handle multiple publishers to the same
subscription, this protocol does by using data contexts in this fashion.  This protocol therefore
supports [pipeline forwarding](#pipeline-forwarding) with **one or more publishers to the same track**.

## Connection Establishment

Peers can be established by either side. Only one peer connection can be used for control peering. If multiple
parallel connections are established, the first connection made will be used for control peering bidirectionally.

Data peering can be used over all connections. The mode of the peering session indicates control verses
data or both. If mode is data peer, the data mode can be unidirectional or bidirectional. If bidirectional,
return data will be sent via the established peering connection. Bidirectional peering is to support 
NAT and firewall use-cases. 

> [!NOTE]
> Data streams are always unidirectional, where the sender sends unidirectional to the remote side of the
> connection. Connections are bidirectional and support unidirectional streams and the control
> bidirectional stream.

A race condition exists if peer connections are initiated by both relays at the same time. In data forwarding, both
connections can be used. For control, the peer that is established first wins and if they both are established at
the same time, the peer that has the **lowest Node ID** wins. The result is that only one peering connection between
two Nodes will be used for control information base messages.

The control peering connection **MUST** remain established. Upon termination, all states associated to/from that peer
session will be removed. For example, all nodes learned/used via this session and all announces/subscribes via
the session will be removed.

## Selection Algorithm

The selection algorithm will evolve over time to include more granular metrics on load, usage, best via relays for
aggregation, and administrative/business policy constraints,...

AI and ML will be used in the very near future to optimize v-relay nodes to inject. The selection
algorithm will be updated to establish the selection of nodes at start and ongoing. For example, 
it is expected and fully supported to inject or change v-relays at any time to mitigate network issues,
load issues, or to aggregate bandwidth based on changing of subscribers and other factors. The
forwarding plane change is ZERO loss. 

In the near future; Reachability is checked and maintained by nodes to know if it can reach v-relays and other e-relays. 
It is also likely that a pre-selection of v-relays by the s-relay will be added to Node information. Node advertisements convey this information via control peering. All nodes know which nodes can reach the s-relay and other relays.
The selection algorithm will use this information to select which node to forward via, which is performed hop-by-hop.


At this time, peering is setup ahead of time by configuration and uses the selection algorithm to find
the shortest and best path. 

```mermaid
---
title: Peer Configuration and Topology with s-relays
---
flowchart TD
    OR(o-relay) -.-> V1[v-relay 1]
    style OR fill:#AED6F1,stroke:#424949,stroke-width:2px,color:#1C2833

    OR --> V2[v-relay 2]
    V1 <-.-> V2

    OR ----> S1(s-relay 1)
    style S1 fill:#A9DFBF,stroke:#424949,stroke-width:2px,color:#1C2833

    V2 --> S2(s-relay 2)
    style S2 fill:#A9DFBF,stroke:#424949,stroke-width:2px,color:#1C2833
```

```mermaid
---
title: Computed best path Topology with s-relays
---
flowchart TD
    OR(o-relay) --> V2[v-relay 2]
    style OR fill:#AED6F1,stroke:#424949,stroke-width:2px,color:#1C2833

    OR ----> S1(s-relay 1)
    style S1 fill:#A9DFBF,stroke:#424949,stroke-width:2px,color:#1C2833

    V2 --> S2(s-relay 2)
    style S2 fill:#A9DFBF,stroke:#424949,stroke-width:2px,color:#1C2833
```

The above is a simple illustration of a computed topology using the initial selection algorithm defined below.
In this example, v-relay-1 is not used because there is a better path to s-relay 1 directly via v-relay 2. 
If peering were to be removed between o-relay and v-relay-2, the selection would use v-relay 1.  


Below is the current implemented selection algorithm:

1. Prefer the shortest path len (number of transit hops)
2. Prefer the lowest sum of sRTT for all paths and the sRTT of the peering session itself

## Data Forwarding

Data is forwarded using a data header that is included in every datagram message and start of every QUIC stream.
QUIC streams enable pipeline support where data is forwarded bytes-in to \[fan-out\] bytes-out. Pipeline forwarding
reduces the end-to-end latency and jitter on data between publisher edge relay to all subscriber edge/stub relays.

### Source Routing

Data forwarding is source routed in a similar fashion as described in [Segment Routing Architecture](https://www.rfc-editor.org/rfc/rfc8402.html).
Data forwarding in MoQT is designed to support fan-out of one or more publishers to one or more subscribers. This
is different from IP forwarding (point to point) in terms of the label stack size. Building a stack of labels
for all target nodes could grow into the thousands with a large number of subscribers spanning thousands
of edge relays. It would not scale to send the set of node IDs in every datagram frame or even start of every QUIC
stream, considering QUIC streams may change often due to group/subgroup changes.

Unlike segment routing where it utilizes a stack of labels/sids, this protocol utilizes
a **subscriber node set (SNS)** that **describes the set of subscriber edge relay (s-relays) node IDs that
need to receive the data**. It does **not describe** the path that the data will traverse. 

The forwarding plane will be established relay by relay based on node advertisements. Subscribe source node id
is lookup to find the best peer based the [selection](#selection-algorithm). Considering the nodes
and some peering are provisioned before any publishers/subscribers connect, the computation of best
peer is done well before any data is being forwarded. Establishing a forwarding plane for a publisher to any set of subscribers happens in microseconds internally and upon first data object, hop-by-hop.  

SNS is exchanged via the control bidirectional stream on the peering session that will be used to receive 
the data. Utilizing the control bidir stream within the peer session ensures scope of the SNS to be within
the peer session to support parallel peer sessions and control information based peering that is not using
the same data path. Each SNS will be assigned an SNS ID that is unique to the session.
The sender generates the SNS ID.

The SNS ID is an `unsigned 32-bit` integer that is a monotonic series increasing.
The ID starts at ONE and can wrap to ONE as needed. Zero is reserved to indicate no ID. The ID can skip
forward but cannot be less than the previous, unless wrap occurs. 

Withdraws remove SNS IDs from active state. States are peer scoped and will be removed upon peer session close.

The design of subscriber node sets is to be fast and lightweight with little control signaling involvement to maintain
state. Node sets can change often based on node (e.g., relay) churn with peering sessions and subscribers. This
will result in the SNS being updated. Using a new ID to replace the previous introduces race conditions where data
could be lost. Instead of replacing the ID, the **same ID is updated** with a new set to allow a smooth transition
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
For example, when subscribes are received by the publisher relay (o-relay) the subscriber information
will indicate source nodes of where that subscribe originated. This node id will be added to a set that
is relative to the peering session that reaches that node based on the [selection algorithm](#selection-algorithm)
This will result in different sets based on selection of which peering session reaches best s-relay nodes that need
to receive the data. In large scale, multiple data peering sessions will be utilized, so the sets will be
a subset of total number of s-relay nodes. STUBS are not included. Only Edge relays are added to
this set.

##### FIB

Nodes are computed ahead of time when node advertisements are received. A map is updated to maintain 
which peers are best to reach the advertised s-relay node. Recall that all nodes are advertised, 
which includes all s-relay nodes. The FIB is maintained real-time upon subscribe advertisement to find the best
peer session (future will establish on-demand peering, including v-relays).

> [!NOTE]
> Subscribes are only conveyed for an s-relay. A s-relay may have many subscribers, but only one peering
> control subscribe is advertised.  This compresses the churn of subscribes of clients by s-relay.

The best peer session is likely to have many s-relays associated to it. On the first subscribe, 
the peer session would have an SNS created with the first s-relay added to it. This will create a data context
as described previously. Upon additional subscribes, the s-relays that are new will be added to the SNS.
Since the same SNS Id is used, no stream changes are needed. Data will continue to flow without interruption
as s-relays are added and removed. 

Via Relays (v-relay) do not participate in announces or subscribes with the exception of being a passthrough for
control peering. For data forwarding, v-relays lookup the best path to reach s-relays using the SNS advertisement.
The best peer session will be found for each s-relay in the advertisement. It's likely the same peer will be used
for many s-relays. Upon finding the best peer, the first s-relay added to the peer session will result
in an out going SNS Id being created with one or more of the SNS advertisement s-relays being added to it. A map is
used to track ingress SNS Id to Egress SNS Id(s) via peer sessions. Only the first creation results in the creation
and update. Data is then forwarded efficiently using the FIB map based on In to Out SNS Ids. 

```mermaid
flowchart TD
    IN_S(In SNS Id) -- "Peer 1" --> ES_1(Out SNS Id)
    IN_S -- "Peer 2" --> ES_2(Out SNS Id)
```

#### SNS Withdrawal

The sender removes the SNS Id by issuing a withdraw. It does this when there are no subscribers left. If the
peering session disconnects, the SNS Ids are removed.

Withdraw SNS information message has the following fields:

| Field  | Description                   |
| ------ | ----------------------------- |
| SNS ID | SNS ID of the entry to remove |


### Pipeline forwarding

Datagram objects are inherently pipelined and no special handling is needed. 

Data forwarded by relays are pipeline forwarded at the stream level. Every byte received is immediately 
sent out to other peers and clients. MoQT publishers encode all needed headers for the subscribing clients
to reassemble. This includes the cache implementation within the relay that acts as a client receiving the data.
Relay nodes do not need to mutate any received data. MoQT data object headers are unchanged from received to
all peers and MoQT sessions. The relay in this sense is forwarding every byte received on a stream to peer
and client sessions on a stream. 

```mermaid
---
title: Pipeline Forwarding
---
flowchart LR
    P(MoQT Publisher) --> DO@{ shape: doc, label: "MoQT Large\nData Object 4000B" }
    style DO fill:#AED6F1,stroke:#424949,stroke-width:1px,color:#1C2833

    DO -- "slice" --> OR[o-relay]

    D1@{ shape: doc, label: "Data Slice 1 1000B" }
    D2@{ shape: doc, label: "Data Slice 2 1000B" }
    D3@{ shape: doc, label: "Data Slice 3 1000B" }
    D4@{ shape: doc, label: "Data Slice 4 1000B" }
    style D1 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style D2 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style D3 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style D4 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5

    OR --> D1
    OR --> D2
    OR --> D3
    OR --> D4

    D1 --> SR
    D2 --> SR
    D3 --> SR
    D4 --> SR

    Dd1@{ shape: doc, label: "Data Slice 1 1000B" }
    Dd2@{ shape: doc, label: "Data Slice 2 1000B" }
    Dd3@{ shape: doc, label: "Data Slice 3 1000B" }
    Dd4@{ shape: doc, label: "Data Slice 4 1000B" }
    style Dd1 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style Dd2 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style Dd3 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5
    style Dd4 fill:#ABEBC6,stroke:#424949,stroke-width:1px,color:#1C2833,stroke-dasharray: 5 5

    SR --> Dd1
    SR --> Dd2
    SR --> Dd3
    SR --> Dd4

    Dd1 --> S(MoQT Subscriber)
    Dd2 --> S
    Dd3 --> S
    Dd4 --> S

    S -- Reassemble --> DdO@{ shape: doc, label: "MoQT Large\nData Object 4000B" }
    style DdO fill:#AED6F1,stroke:#424949,stroke-width:1px,color:#1C2833
```

> The above illustration shows that the orignal slice of data by MTU QUIC SFRAME/DATAGRAM frame segments are forwarded
as is all the way to each subscriber. It is the subscriber that reassembles. This dramatically reduces per hop
delay. End to end latency is close, if not better, than a point to point connection between publisher
and subscriber. 

MoQT Track QUIC stream transitions (e.g, group/subgroup change) received are mirrored to all peer and client sessions. 

Reassembly of data objects is only performed by subscribing clients and relays that implement caching. A relay that
implements caching is acting like a MoQT client that subscribes and publishes. 

In order to support pipeline forwarding there is a need to convey some extra information about how to forward and
start a new stream for egress peers and client sessions.  The [start of stream header](#start-of-stream-data-header) defines
the fields that are added for peering. SNS Id is used to lookup outgoing peer sessions and SNS Ids for each. When
the the SNS Id contains the node itself, then the node lookups up MoQT client sessions that should receive
the data. Looking up MoQT client sessions do not use SNS Id, so the track alias is used instead. 

Client MoQT sessions do not need this header and are sent data minus this header.  

The reason why this extra information is encoded in data start of QUIC stream header instead of SNS advertisement
is to allow SNS to be reused for different track aliases and priorities later.  We will revisit
if some of the start of stream data header info should be moved to SNS advertisement. 


### Matching Subscribes to Announcements

Subscribes are delivered via the control information base forwarding. The publisher edge relay (o-relay) receives
the subscribe via the control peering. Upon subscribe, all announces are checked to see if there is a match.
Matching the announce to subscribe is performed by matching the namespace tuple of hashes and name in the order defined.
Each tuple is matched using an exact match. The order must match. If the announce has less tuples, but matches
all the subscribe tuples up to the announce set, a match is considered found and the publisher will receive
the subscribe. This will trigger SNS and forwarding plane to be built. This is performed based on the subscribe
and publisher accepting that subscribe by sending a subscribe OK back to the s-relay. 

## Messages

This section defines all the peering messages and their encoded wire format. 

The number in parentheses refers to the number of bytes encoded or how it is encoded,
such as [QUIC VAR INT](https://datatracker.ietf.org/doc/html/rfc9000#name-variable-length-integer-enc). If there is
a fixed value, then that is indicated with an equals value.

> [!NOTE]
> All integer and float/double values are encoded in network byte order (e.g., big endian).

### Errors

#### QUIC APP Error Codes

A connection will be closed using [QUIC Application Error Code](https://datatracker.ietf.org/doc/html/rfc9000#section-20.2). The following will be set in the [CONNECTION_CLOSE](https://datatracker.ietf.org/doc/html/rfc9000#name-connection_close-frames) error code or in [RESET_STREAM](https://datatracker.ietf.org/doc/html/rfc9000#name-reset_stream-frames) application error code. 


| Error Code | Description                          |
| ---------- | ------------------------------------ |
| 1          | Graceful close by either side        |
| 2          | Move to alternate relay, GOAWAY      |
| 8          | Not authorized                       |
| 32         | ERROR: Connect message not received  |
| 33         | ERROR: Connect response not received |
| 34         | ERROR: Invalid control message type  |
| 35         | ERROR: Invalid encoding of message   |
| 36         | ERROR: Invalid start of new stream   |
| 128        | Switching to new control stream      |


> [!NOTE]
> This is not a complete list yet. This protocol is still evolving. 

#### Protocol Response Codes

Some control messages may have responses. The response contains a response code as defined below:

| Response Code | Description              |
| ------------- | ------------------------ |
| 0             | No error; successful     |
| 1             | Connection Error         |
| 2             | Not authorized           |
| 3             | Peering mode not allowed |

### Control Messages
Control messages are transmitted over bidirectional streams.  Control messages are never transmitted
over a unidirectional stream. Data objects are only transmitted over unidirectional streams and never
over a bidirectional stream. 

#### Common Control Headers

Common control headers are added to every control message as the first set of headers in the message. 

Data always flows in one direction (uni). **Data objects do not use this common header.**

> [!IMPORTANT]
> Common headers **MUST** be the first added to every message sent via a bidirectional stream.


```
COMMON_CONTROL_HEADER {
    protocol_version(1) = 1,        // Version of this protocol
    message_type(2),                // Message type to follow
    message_length(4),              // Length of the message in bytes that follows
}
```

##### Common Header Message Types

The below table specifies the control message types:

| Type | Name                   | Description                                                                                       |
| ---- | ---------------------- | ------------------------------------------------------------------------------------------------- |
| 1    | CONNECT                | Connect message                                                                                   |
| 2    | CONNECT_RESPONSE       | Connect response message                                                                          |
| 3    | Data Object            | Data object, not sent in control messages directly but is reserved and use only with data objects |
| 4    | NODE_INFO_ADV          | Node information advertisement                                                                    |
| 5    | NODE_INFO_WD           | Node information withdrawn                                                                        |
| 6    | SUBSCRIBE_INFO_ADV     | Subscribe information advertisement                                                               |
| 7    | SUBSCRIBE_INFO_WD      | Subscribe information withdrawn                                                                   |
| 8    | ANNOUNCE_INFO_ADV      | Announce information advertisement                                                                |
| 9    | ANNOUNCE_INFO_WD       | Announce information withdrawn                                                                    |
| 10   | SUBSCRIBE_NODE_SET_ADV | Subscriber node set advertisement                                                                 |
| 11   | SUBSCRIBE_NODE_SET_WD  | SUbscriber node set withdrawn                                                                     |

#### Connect Message

Connect message is the very first message sent by the client side making the connection. The receiving
server side does not send any messages until the client sends a connect message. If the client sends
anything else, the connection is in violation and should be closed with the ERROR code 32.

The client creates a bidirectional stream to be used for control messages. The server will
latch onto this bidirectional stream to send control messages back to the client. The client
can switch bidirectional streams at any time, but MUST use the last (most current) bidirectional
stream going forward. If the previous bidirectional stream is not RESET or FIN closed, it will
be RESET by the server.  

Upon QUIC connection, the client sends a connect message as described below:

```
CONNECT_MESSAGE {
    COMMON_HEADER,

    peer_mode(1),                   // Peer Mode
    self_node_info {                // This node information
        id(8),                      // ID for this node
        type(1),                    // Node type, such as Via=0, Edge=1, Stub=2
        mode(1),                    // Peering Mode        

        contact_len(var-int),       // Length in bytes for contact array of bytes
                                    //   to follow this
        contact,                    // Contact array of bytes, size is contact_len
        longitude(8),               // Double/float value for longitude
        latitude(8),                // Double/float value for latitude

        node_path [                 // array of node path items that are fixed
            {                       //   sized. Currently 16 bytes
                id(8),              // Node ID of the node that forwarded the node
                                    //   info (self node info does not include self)
                srtt_us(8),         // SRTT value in microseconds of the receive peer
                                    //   session of node information
            },
        ]
    }
}
```

The node information is forwarded to all other control peers.

#### Connect Response Message

Connect response is the very first message sent by the server in response to the [CONNECT](#connect-message). 
It is very similar to the connect message except for the following:

* It does not contain peering mode because the client is the one that defines the mode. If the peering mode 
  is not accepted, a response code will indicate that it is not allowed
* It does not contain node information for self if there is an error, indicated by response code

A connect response message MUST be the first message sent by the server. If it is not and/or if a
unidirectional stream is created before receiving the connect response, the connection will be closed
with the error code 33. 

```
CONNECT_RESPONSE_MESSAGE {
    COMMON_HEADER,

    response_code(2),               // Response error code

    [self_node_info {               // Optional self node information, set only if response is successful
        id(8),                      // ID for this node
        type(1),                    // Node type, such as Via=0, Edge=1, Stub=2        

        contact_len(var-int),       // Length in bytes for contact array of bytes
                                    //   to follow this
        contact,                    // Contact array of bytes, size is contact_len
        longitude(8),               // Double/float value for longitude
        latitude(8),                // Double/float value for latitude

        node_path [                 // array of node path items that are fixed
            {                       //   sized. Currently 16 bytes
                id(8),              // Node ID of the node that forwarded the node
                                    //   info (self node info does not include self)
                srtt_us(8),         // SRTT value in microseconds of the receive peer
                                    //   session of node information
            },
        ]
    }]
}
```

The node information is forwarded to all other control peers.

#### Node Information Advertisement Message

Node advertisements are sent upon connection establishment and when there are changes
to node information. Changes in the future will include load, reachability, and more. 

The frequency of node advertisements will be buffered for a configurable short amount of
time (e.g., 2 seconds) to state compress several updates to 1 final state change.  This
ensures that node advertisements do not cause massive churn and excessive forwarding
plane computations. 

Each node that receives the node advertisement will recompute the forwarding plane based on
[selection](#selection-algorithm). 

As described previously, control peering is required to be a persistent connection to either another relay or
to a distributed control node.  This directly is to ensure a stable control plane with little
to no churn in a stable infrastructure.

Each node in the network may receive node information with different values for the same
node Id, such as the path changing. The best node advertisement is selected, the inferior ones are dropped
and not forwarded by the receiving node. 

```
NODE_INFO_ADV {
    COMMON_HEADER,

    node_info {                     // node information
        id(8),                      // Node ID
        type(1),                    // Node type, such as Via=0, Edge=1, Stub=2        

        contact_len(var-int),       // Length in bytes for contact array of bytes
                                    //   to follow this
        contact,                    // Contact array of bytes, size is contact_len
        longitude(8),               // Double/float value for longitude
        latitude(8),                // Double/float value for latitude

        node_path [                 // array of node path items that are fixed
            {                       //   sized. Currently 16 bytes
                id(8),              // Node ID of the node that forwarded the node
                                    //   info (self node info does not include self)
                srtt_us(8),         // SRTT value in microseconds of the receive peer
                                    //   session of node information
            },
        ]
    }
}
```

Node information is sent to all control peers that do not contain the node ID in it's path or back to itself. 

#### Node Information Withdraw Message

A node information withdraw message removes the state of the node information. The forwarding plane
is recomputed to remove the node and select another node for active subscribes. 

The full node information is provided instead of just the ID to support removal of an inferior node
advertisement. 

```
NODE_INFO_WD {
    COMMON_HEADER,

    node_info {                     // node information
        id(8),                      // Node ID
        type(1),                    // Node type, such as Via=0, Edge=1, Stub=2        

        contact_len(var-int),       // Length in bytes for contact array of bytes
                                    //   to follow this
        contact,                    // Contact array of bytes, size is contact_len
        longitude(8),               // Double/float value for longitude
        latitude(8),                // Double/float value for latitude

        node_path [                 // array of node path items that are fixed
            {                       //   sized. Currently 16 bytes
                id(8),              // Node ID of the node that forwarded the node
                                    //   info (self node info does not include self)
                srtt_us(8),         // SRTT value in microseconds of the receive peer
                                    //   session of node information
            },
        ]
    }
}
```

Node information is sent to all control peers that do not contain the node ID in it's path or back to itself. 

#### Subscribe Information Advertisement Message

Subscribe information is originated by an s-relay upon receiving a subscribe from a client
or from a Stub relay. The source node Id is set to self. Sequence number is incremented
for each advertisement and withdraw. Only the s-relay increments the sequence number. 

The relays mainly use the `track_hash` information. The `subscribe_data` is provided
for MoQT compliance as some of the MoQT subscribe data is needed when sending
MoQT subscribe toward the publisher. Other parameters could be used by control
servers to further authorize the subscribe. 

```
SUBSCRIBE_INFO_ADV {
    COMMON_HEADER,

    sequence(2),                // Advertisement sequence number, supporting wrapping
                                //   Only the source node increments this.
    source_node_id(8),          // Source node that originated this
    track_hash {
        namespace_hash(8),      // Hash of namespace
        name_hash(8),           // Hash of name
        full_name_hash(8),      // Combined hash of both namespace and name hashes
    }

    subscribe_data(variable),   // Raw MoQT received subscribe message
}
```

Subscribe information is sent to all control peers that do not contain the source node ID
in it's path or back to itself. 

#### Subscribe Information Withdraw Message

Subscribe information withdraw is to remove the subscribe from the control plane, 
which will trigger a recompute of the data plane. Due to the churn that is seen
with subscribes, sequence number is used to ensure correct state convergence. 

All information is needed in the withdraw to support MoQT compliance on o-relays. 

Withdraw is the same subscribe message as subscribe with the exception of the type
being withdraw. 

```
SUBSCRIBE_INFO_WD {
    COMMON_HEADER,

    sequence(2),                // Advertisement sequence number, supporting wrapping
                                //   Only the source node increments this.
    source_node_id(8),          // Source node that originated this
    track_hash {
        namespace_hash(8),      // Hash of namespace
        name_hash(8),           // Hash of name
        full_name_hash(8),      // Combined hash of both namespace and name hashes
    }

    subscribe_data(variable),   // Raw MoQT received subscribe message
}
```

Subscribe information is sent to all control peers that do not contain the source node ID
in it's path or back to itself. 


#### Announce Information Advertisement Message

Announce information is advertised by Stub relays to o-relays only. o-relays state maintain
this information for MoQT compliance.  Announce information is not sent to
other control peers. In the future, we may want to send it, but at this time
it is not needed by the LAPS peering protocol. 

Announce information is needed by the o-relay to know which client session should receive
the MoQT subscribe to start the flow of publish track data. When a Stub relay is used, 
the Stub appears as a client to the o-relay and the subscribe will be sent to the Stub.
The stub will then send the MoQT subscribe to the publisher. 

Subscribes are [matched to announce](#matching-subscribes-to-announcements) based on the
the `track_full_name_hash` data.  

```
ANNOUNCE_INFO_ADV {
    COMMON_HEADER,

    source_node_id(8),          // Source node that originated this
    track_full_name_hash {
        namespace_tuple_hashes [
            tuple_hash(8),      // Hash for the namespace tuple at index X
        ]

        name_hash(8),           // Hash of name
    }
}
```

#### Announce Information Withdraw Message

Announce withdraw happens with MoQT client unannounces.  This will trigger a withdraw
of the announce information. 

```
ANNOUNCE_INFO_WD {
    COMMON_HEADER,

    source_node_id(8),          // Source node that originated this
    track_full_name_hash {
        namespace_tuple_hashes [
            tuple_hash(8),      // Hash for the namespace tuple at index X
        ]

        name_hash(8),           // Hash of name
    }
}
```

#### Subscribe Node Set Advertisement Message

Subscribe node set is advertised by the sending peer to the remote side of the peer. It is
consumed by the receiving peer and not propagated to any other peer. It is session specific.

Upon receiving the SNS advertisement, the receiving peer will look at the `nodes` set
and create egress SNS advertisements to other peers based on [selection](#selection-algorithm)
for best node to reach s-relay node id.  If self is in the `nodes` set, then MoQT
 forwarding will take place to forward to a directly attached client or Stub relay. 

> [!IMPORTANT]
> SNS advertisements are control messages, but they are sent on the data peer bidir stream that will
> be receiving the data. They are not sent to control peering sessions.

```
SUBSCRIBE_NODE_SET_ADV {
    COMMON_HEADER,

    id(4),              // Subscribe Node Set (SNS) Id

    nodes [             // Set of s-relay nodes to receive data
        node_id(8),     // Node Id of the s-relay to receive data
    ]
}
```

> [!NOTE]
> `nodes` are a set of s-relay nodes that should receive the data. It is not a set
> of nodes to traverse to reach a destination. 

#### Subscribe Node Set Withdraw Message

Considering SNS is session specific, upon disconnect all SNS states for the session will be
removed. This will trigger removing any egress SNS advertisements to other peers. A graceful
removal is done via a withdraw message to remove an SNS Id.  Upon receiving the withdraw,
egress peer SNS states will be cleaned up. 

```
SUBSCRIBE_NODE_SET_WD {
    COMMON_HEADER,

    id(4),              // Subscribe Node Set (SNS) Id
}
```

Withdraw only needs to withdraw the SNS Id, the node list is moot. 

### Data Object Messages

As stated previously, data objects are sent via unidirectional QUIC streams or datagrams, never
via bidirectional stream. This allows for the protocol to correctly assume that a bidirectional
stream is control messaging, while anything else is data object forwarding. 

##### Data Object Common Header

Data objects share a common header for the start of every data object.  

```
DATA_OBJECT_COMMON_HEADER {
    header_length(1),               // Length in bytes for all data object headers
    type(1),                        // Data object type
}
```

###### Data Object Types 

Below defines the data object types. These types are used to determine which headers
are included. 

| Type | Name            | Description                                           |
| ---- | --------------- | ----------------------------------------------------- |
| 0    | DATAGRAM        | Datagram object                                       |
| 1    | EXISTING_STREAM | Data object within an existing stream                 |
| 2    | NEW_STREAM      | Data object that is the first object via a new stream |


#### Datagram Objects

Datagram objects are limited to MTU, which may be up to 64K or as little as 1280 bytes.  Datagram
suffers from the problem of MTU not being equal hop-by-hop. If the publisher supports 64K and the subscriber supports only 1280, then the datagram object will be dropped on initial
transmission because it is too large.  This is an issue with MoQT not supporting datagram fragmentation.
The limitation is not with this peering protocol. 

```mermaid
---
title: Broken datagram forwarding due to MTU
---
flowchart LR
    P(Publisher) -- MTU/GSO 64K --> OR[o-relay]
    OR -- MTU 9000 --> SR[s-relay]
    SR -- MTU 1480 --> S(Subscriber)

    linkStyle 1 stroke:#ff3,stroke-width:4px,color:red;
    linkStyle 2 stroke:#ff3,stroke-width:4px,color:red;
```

In the above diagram, it illustrates that o-relay will fail to send the publisher datagram
because it's too large to send to the s-relay based on egress MTU being less than the receiving side.


```mermaid
---
title: Good datagram flow with MTU
---
flowchart LR
    P(Publisher) -- MTU 1500 --> OR[o-relay]
    OR -- MTU 9000 --> SR[s-relay]
    SR -- MTU 1500 --> S(Subscriber)
```

The above diagram illustrates that the o-relay receives the data using MTU 1500 and the
s-relay is also using MTU 1500. The connection between the o-relay and s-relay is 9000, 
which is greater than the received MTU. This works because the original datagram
message is not to large to be transmitted egress all the way to the subscriber. 

> [!NOTE]
> For now, there is no fragmentation support for datagram. A datagram message must be complete
(headers and data) on transmit so the receiving side can receive the complete object. 

```
DATAGRAM_DATA_OBJECT {
    DATA_OBJECT_COMMON_HEADER,

    sns_id(8),                  // SNS ID that defines target s-relays
    track_full_name_hash(8),    // Track full name hash
    data_length(var-int),       // Var-int data length in bytes
    data(...)                   // Data that follows, will be size of data_length   
}
```

`data_length` is not needed for datagram as the full message must be complete, no fragments. It
is here to support fragments in the future and to keep it consistent with stream objects.

#### Start of New Stream Data Object

New unidirectional QUIC streams must start with a `NEW_STREAM_DATA_OBJECT`. This will include
additional headers that are required on start of the new stream and are shared
by all subsequent stream data objects. 

> [!IMPORTANT]
> It is a stream error if the stream does not start with a new stream data object. The stream will
be reset with error code 36 to indicate this issue.

```
NEW_STREAM_DATA_OBJECT {
    DATA_OBJECT_COMMON_HEADER,

    sns_id(8),                  // SNS ID that defines target s-relays
    track_full_name_hash(8),    // Track full name hash
    priority(1),                // Priority of the stream
    ttl(4),                     // TTL in microseconds to be applied for
                                //   received data objects on stream

    data_length(var-int),       // Var-int data length in bytes
    data(...)                   // Data that follows, will be size of data_length   
}
```

Priority is used when creating egress streams to peering sessions as well as to MoQT clients. 

TTL is applied to all objects received via the stream. The TTL time starts upon receiving
each object. In this sense, each object received will not expire till TTL time has elapsed
after receiving the data object. 

#### Subsequent Stream Data Objects

Subsequent stream data objects have a shortened header considering the start of stream
object conveyed information that pertains to all data objects in the stream. 


```
EXISTING_STREAM_DATA_OBJECT {
    DATA_OBJECT_COMMON_HEADER,

    data_length(var-int),       // Var-int data length in bytes
    data(...)                   // Data that follows, will be size of data_length   
}
```

## Considerations

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

### Unsubscribe/Subscribe Misuse

The last MoQT subscriber unsubscribe can result in many relays having to change states. If the subscriber is
repeatedly unsubscribing and subscribing, it will result in a ripple effect over many relays, which
causes unnessary churn in the relay network. To mitigate this churn, unsubscribes in peering could be
scheduled in the future based on some configurable time, such as 5 seconds. If a MoQT subscriber
subscribes again before the scheduled unsubscribe being sent, it would simply be canceled and
data would continue as if there was no unsubscribe. Only the MoQT client would be the one that
would stop and start repeatedly. The churn of misuse with unsubscribe and subscribe would
be contained to the s-relay instead of spreading the churn to many other relays.

## Message Flows

This section goes over various message sequence flows.  The flows
are at the protocol message forwarding level. Component level within
the relay processing and implementation is not described here. 

### Connection Establishment

All relays can make out going connections to other relays. Below illustrates
the message flow upon making a connection. 

```mermaid
---
title: Connection Establishment
---
sequenceDiagram
    participant R1 as Relay 1(node)
    participant R2 as Relay 2 (node)
  

    par Initiate Connection
        R1 ->> R2: Create QUIC BI-DIR Stream        
        R1 ->> R2: CONNECT(node_info=R1)
    end

    par Accept Connection    
        note right of R2: Latch onto BI-DIR stream
        R2 --> R2: Authorization
        R2 ->> R1: CONNECT_RESPONSE(node_info=R2)
    end

    note right of R1: Sync information bases
    R1 -->> R2: NODE_INFO_ADV(node-x)
    R1 -->> R2: NODE_INFO_ADV(node-y)

    R2 -->> R1: NODE_INFO_ADV(node-a)
    R2 -->> R1: NODE_INFO_ADV(node-b)

    R1 -->> R2: SUBSCRIBE(1)
    R1 -->> R2: SUBSCRIBE(2)

    R2 -->> R1: SUBSCRIBE(3)
    R2 -->> R1: SUBSCRIBE(4)
```

The sync forwarding is asymmetric and does not have any specific order of who sends
which first. Both sides will normalize and compute best paths.  In this sense,
it is a dump of the information bases to each side, where only the best (aka used) is sent. 


### Node Advertisement

Node advertisements are always sent via connect and connect response messages.  On connect, if the initiating
node is a Stub, the node information is not advertised to any other node. Only node types Edge and Via are
advertised to other peers. 

The below process is followed upon receiving the node information from Edge/Via node types.

```mermaid
---
title: Node Information Receive Processing
---

flowchart LR
    NI@{ shape: manual-input, label: "Node Info Received"} --> L@{ shape: notch-pent, label: "For Each Peer" }
    L --> SK1@{ shape: diamond, label: "ID in Path" }
    SK1 -- "YES" --> END@{ shape: dbl-circ, label: "End" }
    SK1 -- "NO" --> SK2@{ shape: diamond, label: "ID matches\npeer" }
    SK2 -- "YES" --> END
    SK2 -- "NO" --> SK3@{ shape: diamond, label: "Info New or Better" }
    SK3 -- "YES" --> C[["Compute Best Peer"]]
    SK3 -- "NO" --> END
    C --> A[["Advertise"]]
    A -- Next Peer --> L
```

```mermaid
---
title: Node Information Advertisement 
---
sequenceDiagram
    actor R1 as e-relay 1
    participant R2 as e-relay 2
    participant R3 as e-relay 3
    
    R1 ->> R2: NODE_INFO_ADV(R1) srtt=50ms NP=[]
    note right of R2: R1 distance is 0 with SRTT of 50ms
    R2 ->> R2: Process
    R2 ->> R3: NODE_INFO_ADV(R1) NP=[{R2, 50}] srtt=20ms
    R3 ->> R3: Process
    note right of R3: R1 distance is 1 with SRTT of 70ms
```

### Subscribe Advertisement

Subscribes are sent to all control peers. The following process is followed upon receiving the subscribe
and sending it out to other peers. 

```mermaid
---
title: Subscribe Information Receive Processing
---

flowchart LR
    NI@{ shape: manual-input, label: "Subscribe Info Received"} --> SC@{ shape: notch-pent, label: "SEQ gt previous" }
    SC -- "YES" --> ISN@{ shape: diamond, label: "Info New or Better" }
    SC -- "NO" --> END@{ shape: dbl-circ, label: "End" }
    ISN -- "NO" --> END
    ISN -- "YES" --> C[["Compute Best Peer"]]
    C --> L@{ shape: notch-pent, label: "For Each Peer" }

    L --> SK1@{ shape: diamond, label: "SRC ID in Path" }
    SK1 -- "YES" --> END

    SK1 -- "NO" --> SK2@{ shape: diamond, label: "SRC ID matches\npeer" }
    SK2 -- "NO" --> A[["Advertise"]]
    SK2 -- "YES" --> END
    A -- Next Peer --> L
```

```mermaid
---
title: Subscriber Information Advertisement
---
sequenceDiagram
    actor S as MoQT Subscriber
    participant SR as s-relay
    participant OR as o-relay
    actor P as MoQT Publisher

    S ->> SR: SUBSCRIBE(abc)
    SR ->> SR: Process
    SR ->> OR: SUBSCRIBE_INFO_ADV(abc)
    OR ->> OR: Process
    note right of OR: Upon receiving, do announce matching
    OR ->> P: SUBSCRIBE(abc)
    note right of OR: MoQT exchanges
    P -->> OR: MoQT Data Object(...)
    OR --> SR: DATA_OBJECT(...)
    SR --> S: MoQT Data Object(...)

```

### TODO Implementation

Items to be implemented still

* Stub relay type with configuration option
* Via relay type with configuration option
* Distributed control peering servers
* Add dynamic peering
* Add reachability probing and detection of location
* Update selection algorithm to include dynamic peering as well as load and reachability
* Add administrative policy support

