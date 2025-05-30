cmake_minimum_required(VERSION 3.7)

set(WHIP_UWP_RESOURCES
	uwp/offerer/Logo.png
	uwp/offerer/package.appxManifest
	uwp/offerer/SmallLogo.png
	uwp/offerer/SmallLogo44x44.png
	uwp/offerer/SplashScreen.png
	uwp/offerer/StoreLogo.png
	uwp/offerer/Windows_TemporaryKey.pfx
)

set(WHEP_UWP_RESOURCES
	uwp/answerer/Logo.png
	uwp/answerer/package.appxManifest
	uwp/answerer/SmallLogo.png
	uwp/answerer/SmallLogo44x44.png
	uwp/answerer/SplashScreen.png
	uwp/answerer/StoreLogo.png
	uwp/answerer/Windows_TemporaryKey.pfx
  )

# fetch repo for http server in c++
include(FetchContent)
FetchContent_Declare(
  cpp_http_server_lib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.20.1
)
FetchContent_MakeAvailable(cpp_http_server_lib)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(atachannel-whip whip.cpp ${OFFERER_UWP_RESOURCES})
else()
	add_executable(datachannel-whip whip.cpp)
endif()

set_target_properties(datachannel-whip PROPERTIES
    CXX_STANDARD 17
    OUTPUT_NAME whip)

set_target_properties(datachannel-whip PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.whip)



if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-whep whep.cpp ${OFFERER_UWP_RESOURCES})
else()
	add_executable(datachannel-whep whep.cpp)
endif()

set_target_properties(datachannel-whep PROPERTIES
    CXX_STANDARD 17
    OUTPUT_NAME whep)

set_target_properties(datachannel-whep PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.whep)



find_package(Threads REQUIRED)

# set include dirs
target_include_directories(datachannel-whip PRIVATE ${cpp_http_server_lib_SOURCE_DIR})
target_include_directories(datachannel-whep PRIVATE ${cpp_http_server_lib_SOURCE_DIR})

# link libs
target_link_libraries(datachannel-whip LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)
target_link_libraries(datachannel-whep LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json)


if(MSVC)
	add_custom_command(TARGET datachannel-whip POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-whip>
	)
	add_custom_command(TARGET datachannel-whep POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-whep>
	)
endif()
