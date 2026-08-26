cmake_minimum_required(VERSION 3.13)

add_executable(datachannel-turn-dtls-deadlock main.cpp)

set_target_properties(datachannel-turn-dtls-deadlock PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME turn-dtls-deadlock)

find_package(Threads REQUIRED)
target_link_libraries(datachannel-turn-dtls-deadlock LibDataChannel::LibDataChannel Threads::Threads)

if(WIN32)
	# Static libraries do not carry their own link dependencies, so when
	# libdatachannel and its deps are linked statically the final executable
	# must resolve the Windows system libs they reference: OpenSSL/libcrypto
	# (crypt32, ws2_32), libjuice (ws2_32/iphlpapi/bcrypt) and usrsctp (ws2_32).
	# CMake usually propagates these via imported/in-tree target interfaces,
	# but that is not guaranteed when OpenSSL is supplied as raw .lib paths
	# (e.g. the UnrealEnginebundled OpenSSL build script). These are system import libs,
	# so it is always safe and harmless to list explicitly on Windows:
	# duplicates are ignored and only referenced objects get pulled in.
	target_link_libraries(datachannel-turn-dtls-deadlock crypt32 ws2_32 iphlpapi bcrypt)
endif()

if(MSVC AND BUILD_SHARED_LIBS)
	add_custom_command(TARGET datachannel-turn-dtls-deadlock POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:datachannel-turn-dtls-deadlock>
	)
endif()