if(BUILD_TESTING AND LAPS_BUILD_TESTS)
    add_subdirectory( doctest )
endif()

set(SPDLOG_USE_STD_FORMAT ON CACHE INTERNAL "" FORCE)
add_subdirectory( libquicr )

if (LAPS_WITH_MOXYGEN)
    set(BUILD_PICOQUIC OFF CACHE INTERNAL  "" FORCE)
    set(BUILD_TESTS OFF CACHE INTERNAL  "" FORCE)
    list(APPEND _FETCHCONTENT_PROVIDED_DEPS picoquic)
    add_subdirectory(moxygen/standalone)
endif()
