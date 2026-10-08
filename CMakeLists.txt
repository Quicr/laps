cmake_minimum_required(VERSION 3.16)

# Option to control component installation
option(LAPS_INSTALL "Install the laps Components" ON)

option(LAPS_BUILD_TESTS "Build Tests laps" OFF)

option(LAPS_USE_BROTLI "Enable picotls Brotli TLS certificate compression (libbrotlidec)" OFF)
set(QUICR_USE_BROTLI "${LAPS_USE_BROTLI}" CACHE BOOL "Enable picotls Brotli TLS certificate compression" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set (CMAKE_POLICY_VERSION_MINIMUM 3.5)

###
### Define the project name and version
###       Version will be added to version output of apps and library
project(laps
        VERSION 0.18.24
        DESCRIPTION "Latency Aware Publish/Subscribe"
        LANGUAGES CXX)

##
## CPM
##
if (NOT DEFINED ENV{IDF_PATH})
    set (CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
endif()

include(CPM)

##

configure_file( version_config.h.in ${CMAKE_BINARY_DIR}/generated/version_config.h )
include_directories( ${CMAKE_BINARY_DIR}/generated/ ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_compile_options(-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)

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

# quicr/utilities/format.h selects std::format or {fmt} with QUICR_HAS_STD_FORMAT.
# libquicr only adds that definition under its own directory, so relay sources that
# include quicr headers have to repeat the same choice.
if(QUICR_HAS_STD_FORMAT)
    add_compile_definitions(QUICR_HAS_STD_FORMAT)
endif()

# spdlog compiles a bundled copy of fmt by default. libquicr also links fmt::fmt
# when std::vformat is unavailable (typical on libstdc++ / GCC < 13), so linking
# both into lapsRelay produces duplicate fmt::v12 symbols on Linux.
if(TARGET fmt::fmt)
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use external fmt; avoid duplicate symbols with libquicr" FORCE)
endif()
CPMAddPackage("gh:gabime/spdlog@1.17.0")

###
### Build relay
###
add_subdirectory(src)

###
### Tests
###
include(CTest)

if(BUILD_TESTING AND LAPS_BUILD_TESTS)
    add_subdirectory(test)
endif()
