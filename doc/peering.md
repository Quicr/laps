# LAPS Peering Protocol and Architecture

Peering is between relays. Either side can establish a peering connection.

Relay to relay connectivity implements custom peering protocol to interconnect a global network of relays supporting
scale into hundreds of millions of active tracks with any combination of publishers and subscribers.

The high level topology looks like the following:
```mermaid
---
title: High Level Topology
---
flowchart TD
    P([Publisher]) --> STUB1(Stub Relay 1)
    STUB1 --> E1[Edge Relay 1]
    STUB1 -- Best --> E3[Edge Relay 3]
    E1 --> E2[Edge Relay 2]
    E1 ----> S1([Subscriber 1])
    E2 --> STUB2(STUB 2)
    STUB2 --> S2([Subscriber 2])
    E2 --> S5([Subscriber 5])
    E1 -. alt .-> E3
    E3 --> S3([Subscriber 3])
    E3 --> S4([Subscriber 4])
    E2 <-. alt .-> E3
 ```


## Peering Modes

Peering connections operate in one of three modes.

### (1) Control Peering

In this mode, only control messages are exchanged. Mostly [Information Base](#information-bases) messages are exchanged
in this mode. This mode allows a single connection serve control plane functions without requiring the control signaling
to follow *possible* data forwarding. Control signaling doesn't often need more than a single plus redudnant connection
for HA, while [data peering](#2-data-peering) messages can benefit from having more than one QUIC connection to scale
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
one-way or two-way modes. In two-way mode, data peering will forward subscribes matching the connection. 

```mermaid
---
title: Multiple Connections between Relays
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

## Relay types

The type of relay is configured when the relay is started, normally via commandline argument or configuration file.
A relay cannot change type during runtime. It must  reconnect to all peers if the relay type is changed.
The relay advertises its type in the self-node information upon `connect`/`connect_response`. 

Relays are classified into the following three categories.

### (1) Via

A Via relay is used by Edge relays to forward data between other Edge relays. A Via relay is an intermediate relay that
does not participate in various [Information Base](#information-bases) exchanges.

A Via relay:
* Does not need to be part of the `announce` and `subscribe` advertisements and withdrawals selection
  for data-plane forwarding
* It participates in the `node` advertisements and withdrawals to maintain reachability information to all nodes.
* `announce` and `subscribe` advertisements/withdrawals are state maintained, but only to prevent looping and 
  duplication. The retention for this state is set to the maximum convergence time. In this sense, it is a cache
  of seen announces and subscribes in the last several seconds.
* Forwards data streams and datagram objects based on [source routing](#source-routing) using the node information base.
* Traditionally has high vertical scale to support large number of tracks and bandwidth
* Strategically placed geographically near Edge relays to provide an aggregation point for fan-out of many Edge relays.

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
        vNY --> lV[Via Relay]
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

### (2) Edge

Edge relays are core relays that facilitate connections from clients and all [relay types](#relay-types). The Edge
relay is a "**can do it all relay.**" 

These relays often have a lower vertical scale due to them having to do the following extras over Via relays:

* Client connection implementation of MoQT
* Handle several client connections, each of which have different QUIC transport encryption/decryption with varying
  high volume of stream changes based on group and subgroups as defined in MoQT
* Compute subscriber source routing based subscribes (if publisher is attached to edge)
* Perform authorization validation and enforcement
* Perform edge defense with client connections by implementing rate limiting and other DoS mitigations
* On-demand peering establishment based on [selection algorithm](#selection-algorithm)

Edge relays are expected to scale horizontally in a stateless fashion, supporting scale up and scale down based
on demand in the region. Edge relays can be "right-sized" to fit the need of subscriber and publisher demand.

Edges relays are often placed behind network load balancers, which can support **anycast** for both client
and peering connections.

```mermaid
---
title: Edge relay behind Load Balancer (e.g., Anycast)
---
flowchart TD
    P([Publisher]) --> LB((NLB/Anycast))
    S([Subscriber]) --> LB
    OE[Other Edge Relay] --> LB
    V[Via Relay] --> LB

    subgraph Load Balanced
        LB --> E1[Edge Relay 1]
        LB --> E2[Edge Relay 2]
        LB --> E3[Edge Relay 3]
    end
```

### (3) Stub

Stub relays only connect to one or more upstream Edge relays. Primary use-case for a Stub relay
is to fan-out subscribers and to take advantage of local termination at the access edge. For example, residential
access routers, branch offices, NAT routers, access points. 

Stub relays have the following uses, feature capabilities and restrictions:

* Must peer directly with Edge relays
* Can peer to more than one edge relay but **MUST** select outside of this protocol (e.g., manual configuration) which
 relay peer session will be used for subscribes.  Using more than one peer session for subscribes will result in
 duplicate data 
* Will only receive announcements from Edge relay clients
* Will receive subscribes based on the advertised announcements
* Uses very little memory and CPU for peering
* Use-case for more than one peer is to optimize reachability to other edges, which is not often required
* Normally only one edge peering session needed
* Fully supports NAT and return traffic over the established peer connection
* Uses peer mode `Both`, control and data via the same peer connection. 
* It does not connect to a cloud provider control server (not needed)
* Will not accept inbound peer connections and will not implement the full peering protocol
* The Edge relay that the Stub connects to will forward announces and subscribes by marking itself as the source
  node. Stubs are transparent and stateless. Only the Edge relay is aware of the Stub.  Other Edge and Via relays
  are unaware of Stub relays.


## Information Bases

Information bases (IB) hold the control plane information. Information is advertised and withdrawn to maintain the
information basees.  

### Node Information

Node information base (NIB) conveys information about a node itself. It is exchanged in `connect` and `connect_resposne` messages
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
|---------------------------------|-------------------------------------------------------------------------------------|
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
textual scheme.  The textual scheme aids in simplified assignment, automation, 
administrative configuration and troubleshooting. 

**Scheme**: `<high value>:<low value>` textual format. The colon is **REQUIRED**.

The values can be represented as an unsigned 32bit integer or dotted notation of 16bit integers,
such as `<uint16>.<uint16>`.  The values can be mixed in how they are represented. 

**Examples**: All the below are valid configurations of the Node ID
* `1.2:1234`
* `1:1`
* `100.2:9001.2001`
* `123456:789.100`

> Simple deployments may automate the Node ID value using a 64bit hash of the node device or host ID or FQDN (if unique). 

#### Node Path

Node Path is an array of node path items (NPI).  NPIs are appended to the path upon advertisement via the peering
session.  Self node information always is sent with an empty path. For example, the path has a length
of zero when advertising self to a peer session. 

NPIs contain the following fields. 

| Field               | Description                                                                      |
|---------------------|----------------------------------------------------------------------------------|
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

* Node `1:2` - Path length is `0` (direct) and sum(sRTT) is `25ms`
* Node `1:3` - Best is path length `1` and sum(sRTT) of `75ms`; alternate is path length `2` sum(sRTT) of `150ms`
* Node `1:4` - Path length is `2` and sum(sRTT) is `135ms`
* Node `1:5` - Path length is `0` (direct) and sum(sRTT) is `20ms`
* Node `1:6` - Path length is `1` and sum(sRTT) is `60ms`

If a new connection were established between `1:4` and `1:6` with a sRTT of less than `75ms`, `1:4` would
select the path via `1:6` with a path length of `2` and sum(sRTT) of less than `135ms`

### Announcement Information

MoQT announce of namespace tuple and name are advertised in `announce_info` messages. Only the hash of each namespace
item and name are sent. `announce_info` is advertised to all peers. Loop prevention is performed by not forwarding 
`announce_info` messages that have already been seen.  Withdraw of `announce_info` is sent to all peers to remove
an entry upon MoQT unannounce.  

Announce Information (`announce_info`) contains the following:

| Field          | Description                                                                                 |
|----------------|---------------------------------------------------------------------------------------------|
| FullNameHashes | Array of the **namespace tuple** hashes and hash of **name**. Only 64bit hashes are encoded |
| source_node_id | The node ID that received the MoQT announce                                                 |


### Subscriber Information

MoQT subscribe namespace tuple and name are advertised in `subscribe_info` messages. ONly the hash of each namespace
item and name are sent. `subscribe_info` is advertised to the peering session that is the best path to reach
the source node of [matching announce](#matching-subscribes-to-announcements) info. Loop prevention is performed by not
forwarding `subscribe_info` messages that have already been seen. Withdraw of `subscribe_info` is sent by the node that
had the MoQT subscribe (source node).  It is sent 

## Connection Establishment

## Selection Algorithm

## Source Routing

### Optimizing Peering using MoQT GOAWAY

## Matching Subscribes to Announcements

## Pipelining

## Message Flows

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
    
    Note right of R: Using control stream

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

Announces are flooded to all control peers, except to STUB peers.  STUB peers do not require announces as they use
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

