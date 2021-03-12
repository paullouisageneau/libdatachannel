cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

if(WIN32)
	add_executable(datachannel-client-benchmark main.cpp parse_cl.cpp parse_cl.h getopt.cpp getopt.h)
	target_compile_definitions(datachannel-client-benchmark PUBLIC STATIC_GETOPT)
else()
	add_executable(datachannel-client-benchmark main.cpp parse_cl.cpp parse_cl.h)
endif()

set_target_properties(datachannel-client-benchmark PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client-benchmark)
target_link_libraries(datachannel-client-benchmark datachannel nlohmann_json)

if(WIN32)
	add_custom_command(TARGET datachannel-client-benchmark POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-client-benchmark>
	)
endif()
