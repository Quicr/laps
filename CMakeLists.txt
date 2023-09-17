cmake_minimum_required(VERSION 3.16)

# Option to control component installation
option(LAPS_INSTALL "Install the laps Components" ON)

option(LAPS_BUILD_TESTS "Build Tests laps" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

###
### Define the project name and version
###       Version will be added to version output of apps and library
project(laps
        VERSION 0.1.53
        DESCRIPTION "Latency Aware Publish/Subscribe"
        LANGUAGES CXX)

configure_file( version_config.h.in ${CMAKE_BINARY_DIR}/generated/version_config.h )
include_directories( ${CMAKE_BINARY_DIR}/generated/ )

###
### Global Config
###
option(CLANG_TIDY "Perform linting with clang-tidy" OFF)

if(CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY  ${CLANG_TIDY_EXE})
    else()
        message(WARNING "clang-tidy requested, but not found")
    endif()
endif()

###
### Dependencies
###
add_subdirectory(dependencies)

###
### Build relay
###
add_subdirectory(src/lapsRelay)

###
### Build test app
###
add_subdirectory(src/lapsTest)


###
### Tests
###
include(CTest)

if(BUILD_TESTING AND LAPS_BUILD_TESTS)
    add_subdirectory(test)
endif()
