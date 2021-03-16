cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

add_executable(streamer
	main.cpp
	dispatchqueue.cpp
	dispatchqueue.hpp
	h264fileparser.cpp
	h264fileparser.hpp
	helpers.cpp
	helpers.hpp
	opusfileparser.cpp
	opusfileparser.hpp
	fileparser.cpp
	fileparser.hpp
	stream.cpp
	stream.hpp
	ArgParser.cpp
	ArgParser.hpp
)

if(WIN32)
	target_compile_definitions(streamer PUBLIC STATIC_GETOPT)
endif()

set_target_properties(streamer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME streamer)

target_link_libraries(streamer datachannel nlohmann_json)

if(WIN32)
	add_custom_command(TARGET streamer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:streamer>
	)
endif()
