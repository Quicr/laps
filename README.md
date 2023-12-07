# Latency Aware Publish Subscriber (laps)

## Development

### Version
Version is defined in [CMakeLists.txt](CMakeLists.txt) under the **project** command for ```laps```.
In code, the ```version_config.h``` file is generated. Each binary, library and docker container tag will have
this version. 

The last octet of the version should be incremented on any change, unless it's the first version (zero) of a
```major.minor``` change. 

## Build

> **MacOS** command line tools are required. If you get errors on a Mac, run the below.
```
xcode-select --install
```

```
make build
```
### Running tests

Run relay:

```
cd build/src/lapsRelay
openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
    -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.quicr.ctgpoc.com" \
    -keyout server-key.pem -out server-cert.pem

./lapsRelay
```

Run test:

```
cd build/src/lapsRelay
./lapsTest
```

## Build with Docker

To build an image, make sure to have docker running. 

**amd64 image**

```make image-amd64```

**arm64 image**

```make image-arm64```

### Notes on docker build
#### **MUST** run ```git submodule ...``` **BEFORE BUILD**

Before running the docker build, you **MUST** first run the below to update the dependencies.
*This cannot be performed during the docker build process due to the use of **private repos**.*

```
git submodule update --init --recursive
```


```
docker build --no-cache --platform linux/amd64 --tag laps-amd64:latest .
docker build --no-cache --platform linux/arm64/v8 --tag laps-arm64:latest . 
```


## Run with Docker

On Intel:
```
docker run --rm -it quicr/laps-relay:0.1.4-amd64 /bin/bash 
```

On Mac silicon:
```
docker run --rm -it quicr/laps-relay:0.1.4-arm64 /bin/bash 
```

## Run relay with Docker

Build docker images then (replace amd with arm for Apple silicon)

```
docker run --rm -p '33434:33434/udp` -it laps-amd64:latest

```

--- 
## Build laps and Publish to ECR

### Build - amd64

```
make image-amd64
```

### Push Image
> **NOTE**: you can use [duo-sso cli](https://wiki.cisco.com/display/THEEND/AWS+SAML+Setup) to get
> an API KEY/SECRET using your CEC login to AWS.  Length of duration needs to be 3600 seconds or less.

The below will login to ECR, TAG the image build in the previous step and finally push it. 

```
export AWS_ACCESS_KEY_ID=<your key id>
export AWS_SECRET_ACCESS_KEY=<your secret key>

make publish-image
```

---
## Build Raspberry PI Image
The current build supports Raspberry Pi **Debian Lite Bullseye image.   

### (1) Install 64bit OS Lite Debian Bullseye image 
Follow the PI imager instructions at https://www.raspberrypi.com/software/ to install base image. 

### (2) Install/Load the image on Raspberry Pi SD card
Remove the Pi SD card and put it into a reader that you can use to copy the image from your laptop/desktop.

> [!WARNING]
> Before performing the below, backup your existing data on the raspberry Pi if needed.

> [!NOTE]
> The below normally takes less than 15 minutes 

1. Power off your existing raspberry Pi and eject the SD card
2. Connect the SD card to laptop/desktop
3. Run **"Raspberry Pi Imager"**
4. Click "CHOOSE OS"
5. In popup, select "**Raspberry Pi OS (other)**"
6. Scroll down to and select on "**Raspberry Pi OS Lite (64-bit)**"
7. Clock on "CHOOSE STORAGE"
8. Click in the bottom right settings icon
   1. Select and set the hostname you want to use
   2. Select "Enable SSH"
   3. Select "Use password authentication"
   4. Select and set username and password
   5. Scroll down and select and set locale settings
   6. Click "**SAVE**"
9. Click "**WRITE**"
   1. Select "YES" if prompted to apply image customization settings
   2. Select "YES" to erase the current SD disk.
10. The disk should be ejected and put back into the raspberry pi 
11. Power on the raspberry Pi. It will use DHCP with SSH enabled. 

> [!NOTE]
> The raspberry Pi will use DHCP to get an IP address via the wired connection. 
> MDNS can be used to access the device using the **hostname** you configured
> in **step 8(i)**.  For example, my hostname is set to "raspberrypi" using
> the mdns name `ssh pi@raspberrypi.local`
 

### (3) Create binary

You will need docker installed to run the below. 

```
make image-pi
```

### (4) Copy the binary to Pi

The binary will be created as `./build-pi/lapsRelay`

Copy the image:

```
scp ./build-pi/lapsRelay <user>@<ip or hostname.local>:
```

### (5) Create Certificate
This step is only needed to be done once.

SSH to raspberry Pi and run the below:

```
openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
    -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.quicr.ctgpoc.com" \
    -keyout server-key.pem -out server-cert.pem
```

### (6) Run Relay

SSH to raspberry Pi and run the below:

> [!IMPORTANT]
> Cut/paste the below lines at once. They are a single command with the environment variables set. 

```
LAPS_PEERS=relay.us-west-2.quicr.ctgpoc.com \
LAPS_PEER_PORT=33438 \
LAPS_PEER_LISTEN_PORT=33439 \
LAPS_PEER_RELIABLE=1 \
LAPS_DISABLE_SPLITHZ=1 \
LAPS_DISABLE_DEDUP=1 \
LAPS_DEBUG=1 \
   ./lapsRelay
```

The below is example output showing the peering connection established. 

```
2023-09-06T13:05:05.446253 [INFO] [MAIN] main: Starting LAPS Relay (version 0.1.40)
2023-09-06T13:05:05.447533 [INFO] [CMGR] Starting client manager id 33434
2023-09-06T13:05:05.448515 [INFO] [CACHE] Running cache monitor thread
2023-09-06T13:05:05.449103 [INFO] [CMGR] [QSES] [UDP] connect_server: port: 33434 fd: 3
2023-09-06T13:05:05.450885 [INFO] [CMGR] [QSES] [UDP] Starting transport writer thread
2023-09-06T13:05:05.450956 [INFO] [CMGR] [QSES] [UDP] Starting transport reader thread
2023-09-06T13:05:05.453151 [INFO] [PMGR] PeerQueueThread: Running peer manager queue receive thread
2023-09-06T13:05:05.453922 [INFO] [PMGR] PeerManager: Peering manager ID: raspberrypi
2023-09-06T13:05:05.456542 [INFO] [PMGR] watchThread: Running peer manager outbound peer connection thread
2023-09-06T13:05:05.462768 [INFO] [PMGR] [QUIC] Starting server, listening on 127.0.0.1:33439
2023-09-06T13:05:05.462908 [INFO] [PMGR] [QUIC] Starting transport callback notifier thread
2023-09-06T13:05:05.465205 [INFO] [PEER] PeerSession: Starting peer session
2023-09-06T13:05:05.466288 [INFO] [PMGR] [QUIC] Loop got cb_mode: packet_loop_ready, waiting for packets
2023-09-06T13:05:05.466642 [DEBUG] [PMGR] [QUIC] Loop got cb_mode: packet_loop_port_update
2023-09-06T13:05:05.469752 [INFO] [PEER] [QUIC] Connecting to server relay.us-west-2.quicr.ctgpoc.com:33438
2023-09-06T13:05:05.471123 [INFO] [PEER] [QUIC] Starting transport callback notifier thread
2023-09-06T13:05:05.641780 [INFO] [CMGR] Starting client manager id 33435
2023-09-06T13:05:05.642730 [INFO] [PEER] [QUIC] Thread client packet loop for client conn_id: 367839281328
2023-09-06T13:05:05.645660 [INFO] [CMGR] [QSES] [QUIC] Starting server, listening on 127.0.0.1:33435
2023-09-06T13:05:05.646140 [INFO] [CMGR] [QSES] [QUIC] Starting transport callback notifier thread
2023-09-06T13:05:05.646332 [INFO] [CMGR] [QSES] Waiting for server to be ready
2023-09-06T13:05:05.645672 [INFO] [PEER] [QUIC] Loop got cb_mode: packet_loop_ready, waiting for packets
2023-09-06T13:05:05.647269 [INFO] [CMGR] [QSES] [QUIC] Loop got cb_mode: packet_loop_ready, waiting for packets
2023-09-06T13:05:05.647493 [DEBUG] [CMGR] [QSES] [QUIC] Loop got cb_mode: packet_loop_port_update
2023-09-06T13:05:05.647278 [DEBUG] [PEER] [QUIC] Loop got cb_mode: packet_loop_port_update
2023-09-06T13:05:05.672293 [DEBUG] [PEER] [QUIC] Got event 9
2023-09-06T13:05:05.697336 [INFO] [PEER] [QUIC] Connection established to server 35.89.146.164 current_stream_id: 0
2023-09-06T13:05:05.697694 [INFO] [PEER] on_connection_status: Peer conn_id 367839281328 is ready, sending connect message
2023-09-06T13:05:05.722637 [INFO] [PEER] on_recv_notify: Received peer connect OK message from ff8df452105a
```