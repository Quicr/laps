cmake_minimum_required(VERSION 3.16)

# Option to control component installation
option(LAPS_INSTALL "Install the laps Components" ON)

option(LAPS_BUILD_TESTS "Build Tests laps" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set (CMAKE_POLICY_VERSION_MINIMUM 3.5)

###
### Define the project name and version
###       Version will be added to version output of apps and library
project(laps
        VERSION 0.18.14
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

# quicr/log.h picks between std::format and fmt on this definition, which libquicr only defines for its own
# sources. The relay logs through quicr::Logger, so it has to make the same choice libquicr did.
if(HAS_STD_VFORMAT)
    add_compile_definitions(QUICR_HAVE_STD_FORMAT)
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
