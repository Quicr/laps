# Discovery

Discovery is the process and methods used to discover which relay to connect to. Discovery may include
advanced selection to choose the best relay. 

## Relay Discovery

Relays discover themselves via the [Relay peering protocol](relay-protocol.md) using node advertisements.
Relays update their advertisement to indicate various metrics, such as load, network bandwidth, loss, ...
The relays can use out-of-band query API to a control server to query for relays to use. Discovery,
best path selection, and node advertisements are described in the [Relay peering protocol](relay-protocol.md)
document. 

## Client Discovery

Clients make a connection to a relay. The relay that the client makes a connection to is 
determined by one or more of the following methods to discover the relay. 

### Client API Orchestration Service

In large scale deployments, such as Webex, there is a service for clients to query. The service
takes into account many factors, such as which meeting they are joining, who has already joined,
etc. The service then select a relay that is often behind a network (e.g., IP/UDP, possibly QUIC)
load balancer (NLB).  The service often uses Anycast and other databases to select which NLB
should be used for the client based on their past network quality, optimizations, location, etc.
The service may also use mDNS well-known service to support a local relay/NLB. 

The specifies of the API orchestration service is out of scope for this document. 

### Anycast

Anycast is implemented using network load balancers to ensure affinity (aka stickiness). Anycast
is a good solution if cost is not a factor. Anycast provides a highly available and scalable 
solution for clients to connect to a single FQDN. The FQDN often resolves to 2 IPv4 and 2 IPv6
addresses that the client makes a connection to. IP packet forwarding finds the closest 
anycast NLB.  

The specifics of Anycast are out of scope for this document. 

### mDNS

Multicast DNS (mDNS) is described in [RFC6762](https://datatracker.ietf.org/doc/html/rfc6762). mDNS provides both
hostname and service resolution for discovery.  The design of mDNS focuses on discovery in the local area network.
This is often the VLAN, Ethernet segment, or local WiFi network. With WiFi, the SSID maybe used globally, 
but is still localized behind the WiFi access point access network, which is local to the client association.

Hostnames are specific to an endpoint and always have the top level domain (TLD)
`<hostname>.local` .  The hostname resolution is therefore singular. 

Service resolution provides resolution to one or more endpoints listed under the service. In this sense it is
a grouping of endpoints by a name. 

mDNS is limited in scope to provide local access discovery. It works well when there are relays or relay NLBs 
local to the client(s).  Think of discovering printers, airplay, etc. devices on your local network.  A user
opens the client app and then requests to see available devices that can be used, such as find a speaker to
stream to. The client then selects the speaker to use and the application streams via that speaker.  

mDNS pushes selection and logic to the client application to decide which endpoint to connect to based on the
service list of endpoints. This works well when the selection is equal where the user can select
any of the available endpoints. In small LANs, such as residential, coffee shop, ... the selection would be
one of a few to choose from.  In large enterprises such as with floors or buildings, the selection may be a local NLB.  In the future,
the selection may be a DPU enabled switch that the user connects to, which may be the access point
connection to the switched network. 

In this design, `_laps._udp` is the well-known service used.  Relays advertise themselves under this service. 
The user will then select which endpoint to use or selection will use round-robin (RR), similar to
DNS RR. 

#### Linux Configuration

A relay running on Linux can use the following to advertise itself using mDNS under the `_laps._udp` service. 

> [!TIP]
> For debian based deployments, install Avahi using ` apt-get install -y avahi-utils avahi-daemon`

##### Create the laps.service configuration using the following
```sh
cat << END > /etc/avahi/services/laps.service
<?xml version="1.0" standalone='no'?><!--*-nxml-*-->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
<name replace-wildcards="yes">%h</name>
<service>
<type>_laps._udp</type>
<port>33435</port>
</service>
</service-group>
END
```

Avahi should detect this change and load the configuration. If not, use `systemctl restart avahi-daemon` to force
reload. 

> [!NOTE]
> The install script for raspberry Pi will do the install steps listed above to configure avahi.


##### Verify the changes using the below. 

```sh
root@sisu-pi:/home/tim# avahi-browse _laps._udp -r
+  wlan0 IPv6 sisu-pi                                       _laps._udp           local
+  wlan0 IPv4 sisu-pi                                       _laps._udp           local
+   eth0 IPv6 sisu-pi                                       _laps._udp           local
+   eth0 IPv4 sisu-pi                                       _laps._udp           local
+     lo IPv4 sisu-pi                                       _laps._udp           local
=  wlan0 IPv6 sisu-pi                                       _laps._udp           local
   hostname = [sisu-pi.local]
   address = [fe80::2ecf:67ff:fe08:faa3]
   port = [33435]
   txt = []
=  wlan0 IPv4 sisu-pi                                       _laps._udp           local
   hostname = [sisu-pi.local]
   address = [10.42.0.1]
   port = [33435]
   txt = []
=   eth0 IPv6 sisu-pi                                       _laps._udp           local
   hostname = [sisu-pi.local]
   address = [2601:600:c900:cb3a:3f30:cb31:a502:456b]
   port = [33435]
   txt = []
=   eth0 IPv4 sisu-pi                                       _laps._udp           local
   hostname = [sisu-pi.local]
   address = [192.168.1.36]
   port = [33435]
   txt = []
=     lo IPv4 sisu-pi                                       _laps._udp           local
   hostname = [sisu-pi.local]
   address = [127.0.0.1]
   port = [33435]
   txt = []
```


#### MacOS Configuration (developer)

For developers using MacOS, the below can be used to configure MacOS to join the service
for local/development testing. 

##### Advertise self in `_laps._udp` service

In a terminal window, run the below to advertise self. 

```sh
export LAPS_PORT=33435
dns-sd -R "${HOST}" _laps._udp  . ${LAPS_PORT}
```

The above will run till CTRL-C or the terminal window is closed. 

##### Verify that self is being advertised

List the endpoints for `_laps._udp`:

```sh
> dns-sd -B _laps._udp

Browsing for _laps._udp
DATE: ---Sun 16 Feb 2025---
14:26:50.591  ...STARTING...
Timestamp     A/R    Flags  if Domain               Service Type         Instance Name
14:26:50.592  Add        3  24 local.               _laps._udp.          sisu-pi
14:26:50.593  Add        3   1 local.               _laps._udp.          TIEVENS-M-3G3P
14:26:50.593  Add        2  24 local.               _laps._udp.          TIEVENS-M-3G3P
```

Get endpoint details:

```
> dns-sd -L "TIEVENS-M-3G3P" _laps._udp

Lookup TIEVENS-M-3G3P._laps._udp.local
DATE: ---Sun 16 Feb 2025---
14:28:44.493  ...STARTING...
14:28:44.494  TIEVENS-M-3G3P._laps._udp.local. can be reached at TIEVENS-M-3G3P.local.:33435 (interface 1) Flags: 1
14:28:44.494  TIEVENS-M-3G3P._laps._udp.local. can be reached at TIEVENS-M-3G3P.local.:33435 (interface 24)
```



