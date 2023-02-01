file(GLOB SOURCES *.cxx)
file(GLOB HEADER *.h)

add_library( laps  ${SOURCES} ${HEADERS} )

target_compile_options(laps
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(laps
        PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

target_link_libraries(laps
        PUBLIC
        quicr
        quicr-transport
        )

target_include_directories(laps PUBLIC ${PROJECT_SOURCE_DIR}/include)
