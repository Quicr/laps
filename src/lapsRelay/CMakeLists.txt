project( lapsRelay )

file(GLOB RELAY_SOURCES *.cxx)
file(GLOB RELAY_HEADER *.h)

add_executable( lapsRelay  ${RELAY_SOURCES} ${RELAY_HEADERS} )

target_link_libraries( lapsRelay PUBLIC laps quicr quicr-transport )
target_compile_options(lapsRelay
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)
set_target_properties(lapsRelay
        PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)