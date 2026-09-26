cmake_minimum_required(VERSION 3.15)
project(signaling-server-libdatachannel-ws
	VERSION 0.1
	LANGUAGES CXX)

	set(MEDIA_UWP_RESOURCES
	uwp/Logo.png
	uwp/package.appxManifest
	uwp/SmallLogo.png
	uwp/SmallLogo44x44.png
	uwp/SplashScreen.png
	uwp/StoreLogo.png
	uwp/Windows_TemporaryKey.pfx
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(signaling-server-libdatachannel-ws signaling-server.cpp ${MEDIA_UWP_RESOURCES})
else()
	add_executable(signaling-server-libdatachannel-ws signaling-server.cpp)
endif()

set_target_properties(signaling-server-libdatachannel-ws PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME signaling-server)

set_target_properties(signaling-server-libdatachannel-ws PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.signaling-server-libdatachannel-ws)

find_package(Threads REQUIRED)
target_link_libraries(signaling-server-libdatachannel-ws LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)

if(MSVC)
	add_custom_command(TARGET signaling-server-libdatachannel-ws POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:signaling-server-libdatachannel-ws>
	)
endif()
