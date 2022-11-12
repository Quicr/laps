cmake_minimum_required(VERSION 3.10 )

project(laps DESCRIPTION "laps library" )

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(include)
add_subdirectory(src)

target_include_directories ( laps PUBLIC include)

