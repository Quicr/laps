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
docker run --rm -it laps-amd64:latest /bin/bash 
```

On Mac silicon:
```
docker run --rm -it laps-arm64:latest /bin/bash 
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
#DOCKER_TAG=$(git rev-list --count HEAD)
DOCKER_TAG=$(egrep "[ \t]+VERSION[ ]+[0-9]+\.[0-9]+\.[0-9]+" CMakeLists.txt | head -1 | sed -r 's/[ \t]+VERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)/\1/')
docker buildx build --progress=plain \
        --output type=docker --platform linux/amd64 \
        -f Dockerfile -t quicr/laps-relay:${DOCKER_TAG}-amd64 .   
```

### Tag the image
```
#DOCKER_TAG=$(git rev-list --count HEAD)
DOCKER_TAG=$(egrep "[ \t]+VERSION[ ]+[0-9]+\.[0-9]+\.[0-9]+" CMakeLists.txt | head -1 | sed -r 's/[ \t]+VERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)/\1/')

docker tag quicr/laps-relay:${DOCKER_TAG}-amd64 \
    017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64
```

### Login to ECR
Only need to do this once.

> **NOTE**: you can use [duo-sso cli](https://wiki.cisco.com/display/THEEND/AWS+SAML+Setup) to get
> an API KEY/SECRET using your CEC login to AWS.  Length of duration needs to be 3600 seconds or less.

```
export AWS_ACCESS_KEY_ID=<your key id>
export AWS_SECRET_ACCESS_KEY=<your secret key>

docker run --rm \
    -e AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID -e AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY \
    amazon/aws-cli \
    ecr get-login-password --region us-west-1 \
	|  docker login --username AWS --password-stdin 017125485914.dkr.ecr.us-west-1.amazonaws.com
```

### Push Image

```
#DOCKER_TAG=$(git rev-list --count HEAD)
DOCKER_TAG=$(egrep "[ \t]+VERSION[ ]+[0-9]+\.[0-9]+\.[0-9]+" CMakeLists.txt | head -1 | sed -r 's/[ \t]+VERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)/\1/')

docker push 017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64
```
