# Latency Aware Publish/Subscribe (LAPS) Requirements

Below defines the general requirements to run LAPS.

* **Linux** or **MacOS** system (not supported on Windows)
* Architecutre **x86**/amd or **ARM** 32bit or 64bit 
* **1 vCPU** (minimum)
* **50MB RAM** - (minimum)

### MacOS

`MacOS >= 15` is fully supported. Most developers are using MacOS to build and test
LAPS. See the [README.md](../README.md) for more details on building and running
LAPS. 

### Linux Distribution

The binary will run on older versions of Linux distributions, but to build the binary, 
**c++20** is required. `Clang >= 10` and `GCC >= 10` should be used. Current distributions
of debian, ubuntu, and alpine meet these requirements.

### Docker

Docker provides a portable image that can run on various platforms/systems. 

There are two **DockerFiles** provided to build and run LAPS.  

* [Alpine Dockerfile](../Dockerfile) - Alpine build and run image
* [Debian/Ubuntu](../debian.Dockerfile) - Debian (works with Ubuntu) build and run image


The docker image can be deployed on many platforms/systems.  The binary can be extracted to run outside of docker.
Raspberry Pi build is an example of where docker is used to cross compile/build the image
and then run natively without docker on Raspberry Pi. 

Instructions for how to build Raspberry Pi binary is documented in the [README.md](../README.md).

