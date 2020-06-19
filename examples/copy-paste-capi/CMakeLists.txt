cmake_minimum_required(VERSION 3.5.1)
project(offerer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -g -O2")

add_executable(datachannel-copy-paste-capi-offerer offerer.c)
set_target_properties(datachannel-copy-paste-capi-offerer PROPERTIES
	OUTPUT_NAME offerer)
target_link_libraries(datachannel-copy-paste-capi-offerer datachannel)

add_executable(datachannel-copy-paste-capi-answerer answerer.c)
set_target_properties(datachannel-copy-paste-capi-answerer PROPERTIES
	OUTPUT_NAME answerer)
target_link_libraries(datachannel-copy-paste-capi-answerer datachannel)

