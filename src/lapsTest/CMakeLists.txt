cmake_minimum_required(VERSION 3.10 )

project( lapsTest )

set(CMAKE_CXX_STANDARD 14) 
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_executable( lapsTest lapsTest.cxx)

target_link_libraries( lapsTest LINK_PUBLIC laps )
