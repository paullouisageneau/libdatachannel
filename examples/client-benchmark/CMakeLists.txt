cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

set(CLIENT_SOURCES
	main.cpp
	parse_cl.cpp
	parse_cl.h
)

set(GETOPT_SOURCES
	getopt.cpp
	getopt.h
)

if(WIN32)
	add_executable(datachannel-client-benchmark ${CLIENT_SOURCES} ${GETOPT_SOURCES})
	target_compile_definitions(datachannel-client-benchmark PUBLIC STATIC_GETOPT)
else()
	add_executable(datachannel-client-benchmark ${CLIENT_SOURCES})
endif()

set_target_properties(datachannel-client-benchmark PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client-benchmark)

set_target_properties(datachannel-client-benchmark PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.client.benchmark)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-client-benchmark LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)

if(MSVC)
	add_custom_command(TARGET datachannel-client-benchmark POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-client-benchmark>
	)
endif()
