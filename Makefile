# This is just a convenience Makefile to avoid having to remember
# all the CMake commands and their arguments.

# Set CMAKE_GENERATOR in the environment to select how you build, e.g.:
#   CMAKE_GENERATOR=Ninja

BUILD_DIR=build
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
	cmake -S . -B${BUILD_DIR} -DBUILD_TESTING=TRUE -DLAPS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Debug

## build: Builds the project
build: build-prep
	@echo "Skipping submodule update due to possible custom changes"
#	git submodule update --init --recursive
	cmake --build ${BUILD_DIR}

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

## image-arm64: Create ARM64 docker image
image-arm64: docker-prep
	@docker buildx build --progress=plain \
			--output type=docker --platform linux/arm64 \
			-f Dockerfile -t quicr/laps-relay:${DOCKER_TAG}-arm64 .

# TODO: Add ECR targets to publish the image
