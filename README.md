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
The current build supports Raspberry Pi **Debian Lite Bookworm**   

### (1) Install 64bit OS Lite Debian Bookworm 
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
4. Click "CHOOSE DEVICE" and select your Raspberry Pi version of 3, 4 or 5"
4. Click "CHOOSE OS"
5. In popup, select "**Raspberry Pi OS (other)**"
6. Scroll down to and select on "**Raspberry Pi OS Lite (64-bit)**"
7. Clock on "CHOOSE STORAGE"
8. Click in the bottom right settings icon
   1. Select and set the **hostname** to something unique to you
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
> MDNS can be used to access the device using the **hostname** you configured.
> For example, my hostname is set to "tievens-pi" using
> the mdns name `ssh pi@tievens-pi.local`
 

### (3) Creating the binary image

#### Using docker

You will need docker installed to run the below. 

```
make image-pi
```

#### Native build on Pi

##### One-time setup for your Pi
After you have build/installed your Pi, you will need to SSH to it and run the below to setup the build
environment. 

###### (a) APT install dependencies

```
sudo apt-get update
sudo apt-get install -y make cmake openssl golang wget git ca-certificates curl
```

###### (b) Get laps from Github

> [!IMPORTANT]
> Create a key so that you can then update your github account with this key. This is required since laps and
> some of the submoudules are private repos.

```
# Hit enter through the prompts
ssh-keygen

cat ~/.ssh/id_rsa.pub
```

Goto github, under **settings**, then under **SSH and GPG keys**, add a new SSH key with the contents of the above
output of the id_rsa.pub.  

Clone laps 

```
git clone git@github.com:Quicr/laps.git
```

###### (c) Sync laps repo 

> [!WARNING]
> Run the below initially and before doing builds to make sure you have all submodules and latest code
```
git pull
git submodule update --init --recursive
```

###### (d) Build the image

```
# optionally do the below
make cclean 

# Build
export CFLAGS="-Wno-error=stringop-overflow"
export CXXFLAGS="-Wno-error=stringop-overflow"
make build
```

### (4) Copy the latest install-relay.sh script and binary to Pi

The docker built binary will be created as `./build-pi/lapsRelay`
The natively built binary will be created as `./build/lapsRelay`

Copy the image:

```
scp ./build-pi/lapsRelay <user>@<ip or hostname.local>:
```

> [!IMPORTANT]
> Before or after copying the **install-relay.sh** script, you **SHOULD**
> edit the file and update the `Environment=<var=value>` lines based on your config/deployment.
> The defaults should work, but you might need to update them. 

Copy the `install-relay.sh` script:

```
scp ./scripts/install-relay.sh <user>@<ip or hostname.local>:
```

### (5) Run the install-relay script
The install-relay script will install or upgrade existing laps relay. 

> [!NOTE]
> You will be prompted for your password to run `sudo` comments.


```
bash ~/install-relay.sh
```

Example:

```
tim@raspberrypi:~ $ bash install-relay.sh
Shutting down lapsRelay
Updating systemd unit file /etc/systemd/system/laps.service
Copying ~/lapsRelay to /usr/local/laps/lapsRelay
● laps.service - Latency Aware Publish Subscriber (LAPS) relay
     Loaded: loaded (/etc/systemd/system/laps.service; disabled; vendor preset: enabled)
     Active: active (running) since Fri 2024-01-26 16:20:12 PST; 101ms ago
       Docs: https://github.com/quicr/laps
   Main PID: 147676 (lapsRelay)
      Tasks: 12 (limit: 779)
        CPU: 50ms
     CGroup: /system.slice/laps.service
             └─147676 /usr/local/laps/lapsRelay

Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.669225 [DEBUG] [PMGR] [QUIC] packet_loop_port_update
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.670094 [INFO] [PMGR] [QUIC] Starting transport callback notifier thread
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.676149 [INFO] [PEER] [QUIC] Setting idle timeout to 30000ms
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.676290 [INFO] [PEER] [QUIC] Setting wifi shadow RTT to 1us
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.677026 [INFO] [PEER] [QUIC] Connecting to server relay.us-west-2.quicr.ctgpoc.com:33439
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.677423 [INFO] [PEER] [QUIC] Starting transport callback notifier thread
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.745799 [INFO] [PEER] [QUIC] Created new connection context for conn_id: 367620619648
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.746410 [INFO] [PEER] [QUIC] conn_id: 367620619648 data_ctx_id: 1 create new stream with stream_id: 6
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.746557 [INFO] [PEER] Control stream ID 1
Jan 26 16:20:12 raspberrypi lapsRelay[147676]: 2024-01-26T16:20:12.746769 [INFO] [CMGR] Starting client manager id 33435

RUN 'systemctl status laps.service' to get status of service
RUN 'journalctl -u laps.service -f' to tail laps relay log
```

##