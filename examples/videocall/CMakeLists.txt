cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

set(VIDEOCALL_SOURCES
	main.cpp
	dispatchqueue.cpp
	helpers.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	message(error, "not supported")
else()
	add_executable(videocall ${VIDEOCALL_SOURCES})
endif()

set_target_properties(videocall PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME videocall)

set_target_properties(videocall PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.videocall)

find_package(Threads REQUIRED)
target_link_libraries(videocall LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)
