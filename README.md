# Latency Aware Publish Subscriber (laps)

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPROJECT_NAME=laps-relay

cmake --build build 
```

## Build with Docker

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
DOCKER_TAG=$(git rev-list --count HEAD)
docker buildx build --progress=plain \
        --output type=docker --platform linux/amd64 \
        -f Dockerfile -t quicr/laps-relay:${DOCKER_TAG}-amd64 .   
```

### Tag the image
```
DOCKER_TAG=$(git rev-list --count HEAD)
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
DOCKER_TAG=$(git rev-list --count HEAD)
docker push 017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64
```
