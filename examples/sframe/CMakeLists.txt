cmake_minimum_required(VERSION 3.13)

add_executable(datachannel-sframe-sender sender.cpp common.hpp)
set_target_properties(datachannel-sframe-sender PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME sframe-sender)
set_target_properties(datachannel-sframe-sender PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.sframe.sender)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-sframe-sender LibDataChannel::LibDataChannel Threads::Threads)

add_executable(datachannel-sframe-receiver receiver.cpp common.hpp)
set_target_properties(datachannel-sframe-receiver PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME sframe-receiver)
set_target_properties(datachannel-sframe-receiver PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.sframe.receiver)

target_link_libraries(datachannel-sframe-receiver LibDataChannel::LibDataChannel Threads::Threads)

if(MSVC)
	add_custom_command(TARGET datachannel-sframe-sender POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-sframe-sender>
	)
	add_custom_command(TARGET datachannel-sframe-receiver POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-sframe-receiver>
	)
endif()
