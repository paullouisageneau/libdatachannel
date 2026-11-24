# Smoke test for the prebuilt packages attached to releases: builds a small
# consumer against an installed libdatachannel found with find_package().
# Usage: cmake -S .github/prebuild-test -B build -DCMAKE_PREFIX_PATH=<package dir>
cmake_minimum_required(VERSION 3.21...4.4.4)
project(libdatachannel-prebuild-test LANGUAGES CXX)

find_package(LibDataChannel REQUIRED CONFIG)
message(STATUS "Found LibDataChannel ${LibDataChannel_VERSION} in ${LibDataChannel_DIR}")

add_executable(prebuild-test main.cpp)
set_target_properties(prebuild-test PROPERTIES CXX_STANDARD 17)
target_link_libraries(prebuild-test PRIVATE LibDataChannel::LibDataChannel)

if(WIN32)
	# Copy the runtime DLLs of the package next to the executable
	add_custom_command(TARGET prebuild-test POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:prebuild-test> $<TARGET_FILE_DIR:prebuild-test>
		COMMAND_EXPAND_LISTS)
endif()
