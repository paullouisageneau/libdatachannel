cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

set(CLIENT_UWP_RESOURCES
	uwp/Logo.png
	uwp/package.appxManifest
	uwp/SmallLogo.png
	uwp/SmallLogo44x44.png
	uwp/SplashScreen.png
	uwp/StoreLogo.png
	uwp/Windows_TemporaryKey.pfx
)

if(WIN32)
	if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-client main.cpp parse_cl.cpp parse_cl.h getopt.cpp getopt.h ${CLIENT_UWP_RESOURCES})
	else()
	    add_executable(datachannel-client main.cpp parse_cl.cpp parse_cl.h getopt.cpp getopt.h)
	endif()
	target_compile_definitions(datachannel-client PUBLIC STATIC_GETOPT)
else()
	add_executable(datachannel-client main.cpp parse_cl.cpp parse_cl.h)
endif()

set_target_properties(datachannel-client PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client)
target_link_libraries(datachannel-client datachannel nlohmann_json)

if(WIN32)
	add_custom_command(TARGET datachannel-client POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-client>
	)
endif()
