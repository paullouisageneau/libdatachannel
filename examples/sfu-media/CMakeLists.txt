cmake_minimum_required(VERSION 3.7)

set(SFU_MEDIA_UWP_RESOURCES
	uwp/Logo.png
	uwp/package.appxManifest
	uwp/SmallLogo.png
	uwp/SmallLogo44x44.png
	uwp/SplashScreen.png
	uwp/StoreLogo.png
	uwp/Windows_TemporaryKey.pfx
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-sfu-media main.cpp ${SFU_MEDIA_UWP_RESOURCES})
else()
	add_executable(datachannel-sfu-media main.cpp)
endif()
set_target_properties(datachannel-sfu-media PROPERTIES
		CXX_STANDARD 17
		OUTPUT_NAME sfu-media)
target_link_libraries(datachannel-sfu-media datachannel nlohmann_json)

if(WIN32)
	add_custom_command(TARGET datachannel-sfu-media POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-sfu-media>
	)
endif()
