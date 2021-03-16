cmake_minimum_required(VERSION 3.5.1)
project(offerer C)

set(CMAKE_C_STANDARD 11)

add_executable(datachannel-copy-paste-capi-offerer offerer.c)
set_target_properties(datachannel-copy-paste-capi-offerer PROPERTIES
	OUTPUT_NAME offerer)
target_link_libraries(datachannel-copy-paste-capi-offerer datachannel)

add_executable(datachannel-copy-paste-capi-answerer answerer.c)
set_target_properties(datachannel-copy-paste-capi-answerer PROPERTIES
	OUTPUT_NAME answerer)
target_link_libraries(datachannel-copy-paste-capi-answerer datachannel)

if(WIN32)
	add_custom_command(TARGET datachannel-copy-paste-capi-offerer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-copy-paste-capi-offerer>
	)
	add_custom_command(TARGET datachannel-copy-paste-capi-answerer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-copy-paste-capi-answerer>
	)
endif()
