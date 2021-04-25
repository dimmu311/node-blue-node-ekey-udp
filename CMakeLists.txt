cmake_minimum_required(VERSION 3.8)
project(ekey-udp)
set(CMAKE_CXX_STANDARD 17)

set(SOURCE_FILES
        Ekey-Udp.h
        Ekey-Udp.cpp)

set(CMAKE_SHARED_LIBRARY_PREFIX "")
add_library(ekey-udp SHARED ${SOURCE_FILES})
#install(TARGETS ekey-udp DESTINATION /var/lib/homegear/node-blue/nodes/ekey)
add_custom_command(TARGET ekey-udp POST_BUILD COMMAND mv ARGS ekey-udp.so ../ekey-udp.so)