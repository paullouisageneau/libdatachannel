cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

if(WIN32)
add_executable(streamer main.cpp dispatchqueue.cpp dispatchqueue.hpp h264fileparser.cpp h264fileparser.hpp helpers.cpp helpers.hpp opusfileparser.cpp opusfileparser.hpp fileparser.cpp fileparser.hpp stream.cpp stream.hpp ArgParser.hpp ArgParser.cpp)
target_compile_definitions(streamer PUBLIC STATIC_GETOPT)
else()
add_executable(streamer main.cpp dispatchqueue.cpp dispatchqueue.hpp h264fileparser.cpp h264fileparser.hpp helpers.cpp helpers.hpp opusfileparser.cpp opusfileparser.hpp fileparser.cpp fileparser.hpp stream.cpp stream.hpp ArgParser.hpp ArgParser.cpp)
endif()
set_target_properties(streamer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME streamer)

target_link_libraries(streamer datachannel)

target_link_libraries(streamer datachannel nlohmann_json)

