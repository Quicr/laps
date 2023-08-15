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
The code currently requires a modern C++ (gcc/clang) compiler.  RaspberryPI base image is debian bullseye. 

```important
Must upgrade to debian **bookworm**  
```

### (1) Install 64bit OS Lite Debian Bullseye image 
Follow the PI imager instructions at https://www.raspberrypi.com/software/ to install base iamge. 

### (2) Upgrade bullseye to bookworm

Follow the below steps:

```
# Step 1
sudo apt update && sudo apt upgrade -y

# Step 2
sudo apt --purge autoremove

# Step 3
sudo sed -i -r 's/bullseye/bookworm/' /etc/apt/sources.list 

# Step 4
sudo apt update

# Step 5
sudo apt upgrade --without-new-pkgs -y 

# Step 6
sudo apt full-upgrade -y
 
# Step 7
sudo reboot
```

Verify after reboot:

```
lsb_release -d
cat /etc/debian_version
```

### (3) Create binary

You will need docker installed to run the below. 

```
make image-pi
```

### (4) Copy the binary to Pi

The binary will be created as `./build-pi/lapsRelay`

Copy the image:

```
scp ./build-pi/lapsRelay <user>@<pi ip>:
```
