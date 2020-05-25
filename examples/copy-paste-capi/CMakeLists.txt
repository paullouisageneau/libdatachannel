cmake_minimum_required(VERSION 3.5.1)
project(offerer C)


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -g -O2")

add_executable(offerer offerer.c)
target_link_libraries(offerer  datachannel)

add_executable(answerer answerer.c)
target_link_libraries(answerer  datachannel)
