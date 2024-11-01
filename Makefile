# This is just a convenience Makefile to avoid having to remember
# all the CMake commands and their arguments.

# Set CMAKE_GENERATOR in the environment to select how you build, e.g.:
#   CMAKE_GENERATOR=Ninja

BUILD_JOBS?=4
BUILD_DIR?=build
CLANG_FORMAT=clang-format -i

PROJECTNAME := laps

.PHONY: all clean cclean format

# -----------------------------------------
# Help/other targets
# -----------------------------------------
.PHONY: help
help: Makefile
	@echo
	@echo " Choose a command run "$(PROJECTNAME)":"
	@echo
	@sed -n 's/^##//p' $< | column -t -s ':' |  sed -e 's/^/ /'

# -----------------------------------------
# Commands
# -----------------------------------------
all: build

build-prep: CMakeLists.txt
	cmake -S . -B${BUILD_DIR} -DBUILD_TESTING=OFF -DLAPS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Debug

## build: Builds the project
build: build-prep
	@echo "Skipping submodule update due to possible custom changes"
#	git submodule update --init --recursive
	cmake --build ${BUILD_DIR} -j ${BUILD_JOBS}

## clean: cmake clean - soft clean
clean:
	cmake --build ${BUILD_DIR} --target clean

## cclean: Deep clean/wipe
cclean:
	rm -rf ${BUILD_DIR}

## format: Format c/c++ code
format:
	find src -iname "*.h" -or -iname "*.cpp" | xargs ${CLANG_FORMAT}
	find test -iname "*.h" -or -iname "*.cpp" | xargs ${CLANG_FORMAT}

## lint: Lint code
lint:
	reuse lint

## cert: Create cert in build dir
cert:
	@echo "Creating certificate in ${BUILD_DIR}/build/src"
	@openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
        -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=test.m10x.org" \
        -keyout ${BUILD_DIR}/src/server-key.pem -out ${BUILD_DIR}/src/server-cert.pem

# NOTE: This will not work on Windows
DOCKER_TAG := $(shell egrep "[ \t]+VERSION[ ]+[0-9]+\.[0-9]+\.[0-9]+" CMakeLists.txt | head -1 | sed -r 's/[ \t]+VERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)/\1/')
docker-prep:
	@echo "Prep normally requires submodule update, but skipping considering possible custom changes"
#	@git submodule update --init --recursive

## image-amd64: Create AMD64 docker image
image-amd64: docker-prep
	@docker buildx build --progress=plain \
			--output type=docker --platform linux/amd64 \
			-f Dockerfile -t quicr/laps-relay:${DOCKER_TAG}-amd64 .

## image-pi-v7: Create ARM/v7 PI binary in build-pi/
image-pi-32: docker-prep
	@rm -rf build-pi
	@mkdir build-pi
	@docker buildx build --progress=plain \
			--output type=docker --platform linux/arm/v7 \
			-f debian.Dockerfile -t quicr/laps-relay:pi-armv7 .
	@docker rm -f laps-relay-pi
	@docker create --name laps-relay-pi quicr/laps-relay:pi-armv7
	@docker cp laps-relay-pi:/usr/local/bin/lapsRelay ./build-pi/lapsRelay
	@docker rm -f laps-relay-pi

## image-pi: Create PI 64bit binary in build-pi/
image-pi: docker-prep
	@rm -rf build-pi
	@mkdir build-pi
	@docker buildx build --progress=plain \
			--output type=docker --platform linux/arm64 \
			-f debian.Dockerfile -t quicr/laps-relay:pi-arm64 .
	@docker rm -f laps-relay-pi
	@docker create --name laps-relay-pi quicr/laps-relay:pi-arm64
	@docker cp laps-relay-pi:/usr/local/bin/lapsRelay ./build-pi/lapsRelay
	@docker rm -f laps-relay-pi


## image-arm64: Create ARM64 docker image
image-arm64: docker-prep
	@docker buildx build --progress=plain \
			--output type=docker --platform linux/arm64 \
			-f Dockerfile -t quicr/laps-relay:${DOCKER_TAG}-arm64 .

ecr-login:
	@echo "==> Logging into ECR using environment variables AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY"
	@docker run --rm \
    	-e AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID} -e AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY} \
    	amazon/aws-cli \
    	ecr get-login-password --region us-west-1 \
		|  docker login --username AWS --password-stdin 017125485914.dkr.ecr.us-west-1.amazonaws.com

## publish-image: Publish amd64 image to ECR
publish-image: ecr-login
	@echo "==> Tagging docker image to 017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64"
	@docker tag quicr/laps-relay:${DOCKER_TAG}-amd64 \
    	017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64
	@echo "==> Pushing image 017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64 to ECR"
	@docker push 017125485914.dkr.ecr.us-west-1.amazonaws.com/quicr/laps-relay:${DOCKER_TAG}-amd64


