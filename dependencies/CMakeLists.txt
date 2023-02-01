if(BUILD_TESTING AND LAPS_BUILD_TESTS)
    add_subdirectory( doctest )
endif()

add_subdirectory( libquicr )
