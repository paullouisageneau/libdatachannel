cmake_minimum_required(VERSION 3.7)

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
	add_executable(datachannel-media-sender main.cpp ${MEDIA_UWP_RESOURCES})
else()
	add_executable(datachannel-media-sender main.cpp)
endif()

set_target_properties(datachannel-media-sender PROPERTIES
    CXX_STANDARD 17
    OUTPUT_NAME media-sender)

set_target_properties(datachannel-media-sender PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.media-sender)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-media-sender LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)

if(MSVC)
	add_custom_command(TARGET datachannel-media-sender POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-media-sender>
	)
endif()
