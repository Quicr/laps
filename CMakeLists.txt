cmake_minimum_required(VERSION 3.17)

# Option to control component installation
option(laps_INSTALL "Install the laps Components" ON)

option(laps_BUILD_TESTS "Build Tests laps" OFF)


# Define the project name and version
project(laps
        VERSION 0.0.2
        DESCRIPTION "Latency Aware Publish/Subscribe"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file( version_config.h.in ${CMAKE_BINARY_DIR}/generated/version_config.h )
include_directories( ${CMAKE_BINARY_DIR}/generated/ )

add_subdirectory(src/lib/laps)

add_subdirectory(src/lapsTest)
add_subdirectory(src/lapsRelay)

include(CTest)

#if(BUILD_TESTING AND laps_BUILD_TESTS)
#    add_subdirectory(test)
#endif()
