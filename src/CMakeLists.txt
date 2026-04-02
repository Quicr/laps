# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

if(CLANG_TIDY_EXE)
  set_target_properties(quicr PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

set(LAPS_SOURCES
        main.cc
        config.cc
        client_manager.cc
        moq/shim/relay_core.cc
        subscribe_handler.cc
        publish_handler.cc
        fetch_handler.cc
        publish_fetch_handler.cc
        publish_namespace_handler.cc
        quicr_client_manager.cc

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

if (LAPS_MOQ_DUAL_BACKEND)
        list(APPEND LAPS_SOURCES
                moxygen_client_manager.cc
                moxygen_relay_with_policy.cc
                moq/shim/laps_moq_relay_session.cc
                moq/shim/moxygen_moq_server_port.cc
                moq/shim/moxygen_quicr_subscribe_track_consumer.cc
                moq/shim/moxygen_subscribe_to_relay.cc
        )
elseif (LAPS_WITH_MOXYGEN)
        list(APPEND LAPS_SOURCES
                moxygen_client_manager.cc
                moxygen_relay_with_policy.cc
                moq/shim/laps_moq_relay_session.cc
                moq/shim/moxygen_moq_server_port.cc
                moq/shim/moxygen_quicr_subscribe_track_consumer.cc
                moq/shim/moxygen_subscribe_to_relay.cc
        )
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
target_link_libraries(lapsRelay PRIVATE quicr)

if (LAPS_MOQ_DUAL_BACKEND)
        target_compile_definitions(lapsRelay PRIVATE WITH_QUICR WITH_MOXYGEN LAPS_HAVE_DUAL_MOQ_BACKENDS)
        target_link_libraries(lapsRelay PRIVATE
                openmoq_pico_server_transport
                moxygen_moq_relay_session
                moxygen_relay_moq_relay
        )
elseif (LAPS_WITH_MOXYGEN)
        target_compile_definitions(lapsRelay PRIVATE WITH_MOXYGEN)
        target_link_libraries(lapsRelay PRIVATE
                openmoq_pico_server_transport
                moxygen_moq_relay_session
                moxygen_relay_moq_relay
        )
else()
        target_compile_definitions(lapsRelay PRIVATE WITH_QUICR)
endif()

target_include_directories(lapsRelay PRIVATE ${PROJECT_SOURCE_DIR}/src/moq/shim)
