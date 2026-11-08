cmake_minimum_required(VERSION 3.13)

set(WHIP_WHEP_UWP_RESOURCES
	uwp/Logo.png
	uwp/package.appxManifest
	uwp/SmallLogo.png
	uwp/SmallLogo44x44.png
	uwp/SplashScreen.png
	uwp/StoreLogo.png
	uwp/Windows_TemporaryKey.pfx
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
	add_executable(datachannel-whip-whep main.cpp ${WHIP_WHEP_UWP_RESOURCES})
else()
	add_executable(datachannel-whip-whep main.cpp)
endif()

set_target_properties(datachannel-whip-whep PROPERTIES
    CXX_STANDARD 17
    OUTPUT_NAME whip-whep)

set_target_properties(datachannel-whip-whep PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.whip-whep)

find_package(Threads REQUIRED)

target_include_directories(datachannel-whip-whep PRIVATE ${cpp_http_server_lib_SOURCE_DIR})
target_link_libraries(datachannel-whip-whep LibDataChannel::LibDataChannel Threads::Threads)

# The server serves the page from the directory holding the executable, so put a
# copy of index.html there.
add_custom_command(TARGET datachannel-whip-whep POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	"${CMAKE_CURRENT_SOURCE_DIR}/index.html"
	$<TARGET_FILE_DIR:datachannel-whip-whep>
)

if(MSVC)
	add_custom_command(TARGET datachannel-whip-whep POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-whip-whep>
	)
endif()
