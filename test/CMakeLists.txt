# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

add_executable(laps_test
        main.cc
        peering_node.cc
        peering_connect.cc
        peering_subscribe.cc
        peering_announce.cc
        peering_sns.cc
        peering_data_header.cc
        peering_info_base.cc
        track_ranking.cc
        relay_health_check_test.cc

        ../src/peering/messages/connect.cc
        ../src/peering/messages/connect_response.cc
        ../src/peering/messages/node_info.cc
        ../src/peering/messages/subscribe_info.cc
        ../src/peering/messages/announce_info.cc
        ../src/peering/messages/subscribe_node_set.cc
        ../src/peering/messages/data_header.cc
        ../src/peering/info_base.cc
)
target_include_directories(laps_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(laps_test PRIVATE quicr doctest::doctest relay_health_check_lib)

target_compile_options(laps_test
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(laps_test
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

if (CLANG_TIDY_EXE)
    set_target_properties(laps_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(laps_test PRIVATE -Wall -pedantic -Wextra)
endif ()

if (MSVC)
    target_compile_options(laps_test PRIVATE /W4 /WX)
    target_compile_definitions(laps_test _CRT_SECURE_NO_WARNINGS)
endif ()

include(${CMAKE_SOURCE_DIR}/dependencies/doctest/scripts/cmake/doctest.cmake)
doctest_discover_tests(laps_test)

# Scoped test target for relay_health_check helpers — kept separate so it stays
# green even when broader peering tests are temporarily out of sync with
# libquicr's evolving public API.
add_executable(relay_health_check_test
        main.cc
        relay_health_check_test.cc
)
target_include_directories(relay_health_check_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(relay_health_check_test PRIVATE doctest::doctest relay_health_check_lib)
set_target_properties(relay_health_check_test
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)
doctest_discover_tests(relay_health_check_test)
