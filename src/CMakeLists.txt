# SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
# SPDX-License-Identifier: BSD-2-Clause

if(CLANG_TIDY_EXE)
  set_target_properties(quicr PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()


add_executable(lapsRelay
        main.cc
        config.cc
        client_manager.cc
        subscribe_handler.cc
        publish_handler.cc
        fetch_handler.cc

        peering/messages/connect.cc
        peering/messages/connect_response.cc
        peering/messages/subscribe_info.cc
        peering/messages/announce_info.cc
        peering/messages/node_info.cc
        peering/messages/subscribe_node_set.cc
        peering/messages/data_object.cc

        peering/peer_manager.cc
        peering/peer_session.cc
        peering/info_base.cc
)

target_link_libraries(lapsRelay PRIVATE quicr)
target_include_directories(lapsRelay PRIVATE ${PROJECT_SOURCE_DIR}/dependencies/oss )

target_compile_options(lapsRelay
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(lapsRelay
        PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

target_compile_definitions(lapsRelay PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
