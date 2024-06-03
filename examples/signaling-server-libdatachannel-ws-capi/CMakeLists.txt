cmake_minimum_required(VERSION 3.15)
project(signaling-server-libdatachannel-ws-capi
	VERSION 0.1
	LANGUAGES C)

add_executable(signaling-server-capi signaling-server.c)

set_target_properties(signaling-server-capi PROPERTIES
	C_STANDARD 11
	OUTPUT_NAME signaling-server)

find_package(Threads REQUIRED)
target_link_libraries(signaling-server-capi LibDataChannel::LibDataChannel Threads::Threads)

if(MSVC)
	add_custom_command(TARGET signaling-server-capi POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:signaling-server-capi>
	)
endif()
