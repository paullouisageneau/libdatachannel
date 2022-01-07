cmake_minimum_required(VERSION 3.7)

set(OFFERER_UWP_RESOURCES
	uwp/offerer/Logo.png
	uwp/offerer/package.appxManifest
	uwp/offerer/SmallLogo.png
	uwp/offerer/SmallLogo44x44.png
	uwp/offerer/SplashScreen.png
	uwp/offerer/StoreLogo.png
	uwp/offerer/Windows_TemporaryKey.pfx
)

set(ANSWERER_UWP_RESOURCES
	uwp/answerer/Logo.png
	uwp/answerer/package.appxManifest
	uwp/answerer/SmallLogo.png
	uwp/answerer/SmallLogo44x44.png
	uwp/answerer/SplashScreen.png
	uwp/answerer/StoreLogo.png
	uwp/answerer/Windows_TemporaryKey.pfx
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-copy-paste-offerer offerer.cpp ${OFFERER_UWP_RESOURCES})
else()
	add_executable(datachannel-copy-paste-offerer offerer.cpp)
endif()

set_target_properties(datachannel-copy-paste-offerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME offerer)

set_target_properties(datachannel-copy-paste-offerer PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.copypaste.offerer)

target_link_libraries(datachannel-copy-paste-offerer datachannel)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(datachannel-copy-paste-answerer answerer.cpp ${ANSWERER_UWP_RESOURCES})
else()
	add_executable(datachannel-copy-paste-answerer answerer.cpp)
endif()

set_target_properties(datachannel-copy-paste-answerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME answerer)

set_target_properties(datachannel-copy-paste-answerer PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.copypaste.answerer)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-copy-paste-answerer LibDataChannel::LibDataChannel Threads::Threads)

if(MSVC)
	add_custom_command(TARGET datachannel-copy-paste-offerer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-copy-paste-offerer>
	)
	add_custom_command(TARGET datachannel-copy-paste-answerer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-copy-paste-answerer>
	)
endif()
