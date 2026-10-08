cmake_minimum_required(VERSION 3.13)

set(SENDER_UWP_RESOURCES
	uwp/sender/Logo.png
	uwp/sender/package.appxManifest
	uwp/sender/SmallLogo.png
	uwp/sender/SmallLogo44x44.png
	uwp/sender/SplashScreen.png
	uwp/sender/StoreLogo.png
	uwp/sender/Windows_TemporaryKey.pfx
)

set(RECEIVER_UWP_RESOURCES
	uwp/receiver/Logo.png
	uwp/receiver/package.appxManifest
	uwp/receiver/SmallLogo.png
	uwp/receiver/SmallLogo44x44.png
	uwp/receiver/SplashScreen.png
	uwp/receiver/StoreLogo.png
	uwp/receiver/Windows_TemporaryKey.pfx
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-sframe-sender sender.cpp sframecommon.hpp ${SENDER_UWP_RESOURCES})
else()
	add_executable(datachannel-sframe-sender sender.cpp sframecommon.hpp)
endif()

set_target_properties(datachannel-sframe-sender PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME sframe-sender)

set_target_properties(datachannel-sframe-sender PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.sframe.sender)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-sframe-sender LibDataChannel::LibDataChannel Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-sframe-receiver receiver.cpp sframecommon.hpp ${RECEIVER_UWP_RESOURCES})
else()
	add_executable(datachannel-sframe-receiver receiver.cpp sframecommon.hpp)
endif()

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
