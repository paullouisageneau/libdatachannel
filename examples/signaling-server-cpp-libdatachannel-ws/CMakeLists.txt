cmake_minimum_required(VERSION 3.7)

add_executable(signaling-server-cpp signaling-server.cpp)

set_target_properties(signaling-server-cpp PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME signaling-server)

find_package(Threads REQUIRED)
target_link_libraries(signaling-server-cpp LibDataChannel::LibDataChannel Threads::Threads nlohmann_json)

if(MSVC)
	add_custom_command(TARGET signaling-server-cpp POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:signaling-server-cpp>
	)
endif()
