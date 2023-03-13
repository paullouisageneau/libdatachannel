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
		add_executable(datachannel-client ${CLIENT_SOURCES} ${GETOPT_SOURCES} ${CLIENT_UWP_RESOURCES})
	else()
	    add_executable(datachannel-client ${CLIENT_SOURCES} ${GETOPT_SOURCES})
	endif()
	target_compile_definitions(datachannel-client PUBLIC STATIC_GETOPT)
else()
	add_executable(datachannel-client ${CLIENT_SOURCES})
endif()

set_target_properties(datachannel-client PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client)

set_target_properties(datachannel-client PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.client)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-client LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)

if(MSVC)
	add_custom_command(TARGET datachannel-client POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-client>
	)
endif()
