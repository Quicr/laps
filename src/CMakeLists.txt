# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

if(CLANG_TIDY_EXE)
  set_target_properties(quicr PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

set(LAPS_SOURCES
        main.cc
        config.cc
        client_manager.cc
        subscribe_handler.cc
        publish_handler.cc
        fetch_handler.cc
        publish_namespace_handler.cc

        peering/messages/connect.cc
        peering/messages/connect_response.cc
        peering/messages/subscribe_info.cc
        peering/messages/announce_info.cc
        peering/messages/node_info.cc
        peering/messages/subscribe_node_set.cc
        peering/messages/data_header.cc

        peering/peer_manager.cc
        peering/peer_session.cc
        peering/info_base.cc
)

if (${LAPS_MOQ_BACKEND} STREQUAL "QUICR")
        list(APPEND LAPS_SOURCES quicr_client_manager.cc)
elseif (${LAPS_MOQ_BACKEND} STREQUAL "MOXYGEN")
        list(APPEND LAPS_SOURCES moxygen_client_manager.cc)
endif()

add_executable(lapsRelay ${LAPS_SOURCES})
target_include_directories(lapsRelay PRIVATE ${PROJECT_SOURCE_DIR}/dependencies/oss)

target_compile_options(lapsRelay
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >
)

set_target_properties(lapsRelay PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON
)

target_compile_definitions(lapsRelay PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
if (${LAPS_MOQ_BACKEND} STREQUAL "QUICR")
        target_link_libraries(lapsRelay PRIVATE quicr)
        target_compile_definitions(lapsRelay PRIVATE WITH_QUICR)
elseif (${LAPS_MOQ_BACKEND} STREQUAL "MOXYGEN")
        target_compile_definitions(lapsRelay PRIVATE WITH_MOXYGEN)
endif()
