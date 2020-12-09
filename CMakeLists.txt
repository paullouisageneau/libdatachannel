cmake_minimum_required(VERSION 3.7)
project(libdatachannel
	VERSION 0.10.4
	LANGUAGES CXX)
set(PROJECT_DESCRIPTION "WebRTC Data Channels Library")

# Options
option(USE_GNUTLS "Use GnuTLS instead of OpenSSL" OFF)
option(USE_NICE "Use libnice instead of libjuice" OFF)
option(USE_SYSTEM_SRTP "Use system libSRTP" OFF)
option(NO_WEBSOCKET "Disable WebSocket support" OFF)
option(NO_MEDIA "Disable media transport support" OFF)
option(NO_EXAMPLES "Disable examples" OFF)
option(NO_TESTS "Disable tests build" OFF)
option(WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(RSA_KEY_BITS_2048 "Use 2048-bit RSA key instead of 3072-bit" OFF)
option(CAPI_STDCALL "Set calling convention of C API callbacks stdcall" OFF)

if(USE_NICE)
	option(USE_JUICE "Use libjuice" OFF)
else()
	option(USE_JUICE "Use libjuice" ON)
endif()

if(USE_GNUTLS)
	option(USE_NETTLE "Use Nettle in libjuice" ON)
else()
	option(USE_NETTLE "Use Nettle in libjuice" OFF)
	if(NOT USE_SYSTEM_SRTP)
		option(ENABLE_OPENSSL "Enable OpenSSL crypto engine for SRTP" ON)
	endif()
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
	if(MSVC)
		add_definitions(-DNOMINMAX)
		add_definitions(-D_CRT_SECURE_NO_WARNINGS)
		add_definitions(-D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
	endif()
endif()

set(LIBDATACHANNEL_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/candidate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/certificate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/channel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/configuration.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/datachannel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/description.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/dtlssrtptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/dtlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/icetransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/init.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/log.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/message.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/peerconnection.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtcp.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/sctptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/threadpool.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tls.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/track.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/processor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/capi.cpp
)

set(LIBDATACHANNEL_WEBSOCKET_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/base64.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tcptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/verifiedtlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/websocket.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/wstransport.cpp
)

set(LIBDATACHANNEL_HEADERS
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/candidate.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/channel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/configuration.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/configuration.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/datachannel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/description.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtcp.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/include.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/init.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/log.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/message.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/peerconnection.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/queue.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/reliability.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.h
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtp.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/track.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/websocket.hpp
)

set(TESTS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/turn_connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/benchmark.cpp
)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)
add_subdirectory(deps/plog)

option(sctp_build_programs 0)
option(sctp_build_shared_lib 0)
add_subdirectory(deps/usrsctp EXCLUDE_FROM_ALL)
if (MSYS OR MINGW)
	target_compile_definitions(usrsctp PUBLIC -DSCTP_STDINT_INCLUDE=<stdint.h>)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(usrsctp PRIVATE -Wno-error=format-truncation)
endif()
add_library(Usrsctp::Usrsctp ALIAS usrsctp)

if (NO_WEBSOCKET)
	add_library(datachannel SHARED
		${LIBDATACHANNEL_SOURCES})
	add_library(datachannel-static STATIC EXCLUDE_FROM_ALL
		${LIBDATACHANNEL_SOURCES})
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=0)
else()
	add_library(datachannel SHARED
		${LIBDATACHANNEL_SOURCES}
		${LIBDATACHANNEL_WEBSOCKET_SOURCES})
	add_library(datachannel-static STATIC EXCLUDE_FROM_ALL
		${LIBDATACHANNEL_SOURCES}
		${LIBDATACHANNEL_WEBSOCKET_SOURCES})
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=1)
endif()

set_target_properties(datachannel PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)
set_target_properties(datachannel-static PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_include_directories(datachannel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel PUBLIC Threads::Threads plog::plog)
target_link_libraries(datachannel PRIVATE Usrsctp::Usrsctp)

target_include_directories(datachannel-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-static PUBLIC Threads::Threads plog::plog)
target_link_libraries(datachannel-static PRIVATE Usrsctp::Usrsctp)

if(WIN32)
	target_link_libraries(datachannel PUBLIC ws2_32) # winsock2
	target_link_libraries(datachannel-static PUBLIC ws2_32) # winsock2
endif()

if(NO_MEDIA)
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=0)
else()
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=1)
	if(USE_SYSTEM_SRTP)
		find_package(SRTP REQUIRED)
		if(NOT TARGET SRTP::SRTP)
			add_library(SRTP::SRTP UNKNOWN IMPORTED)
			set_target_properties(SRTP::SRTP PROPERTIES
				INTERFACE_INCLUDE_DIRECTORIES ${SRTP_INCLUDE_DIRS}
				IMPORTED_LINK_INTERFACE_LANGUAGES C
				IMPORTED_LOCATION ${SRTP_LIBRARIES})
		endif()
		target_compile_definitions(datachannel PRIVATE RTC_SYSTEM_SRTP=1)
		target_compile_definitions(datachannel-static PRIVATE RTC_SYSTEM_SRTP=1)
		target_link_libraries(datachannel PRIVATE SRTP::SRTP)
		target_link_libraries(datachannel-static PRIVATE SRTP::SRTP)
	else()
		add_subdirectory(deps/libsrtp EXCLUDE_FROM_ALL)
		target_compile_definitions(datachannel PRIVATE RTC_SYSTEM_SRTP=0)
		target_compile_definitions(datachannel-static PRIVATE RTC_SYSTEM_SRTP=0)
		target_link_libraries(datachannel PRIVATE srtp2)
		target_link_libraries(datachannel-static PRIVATE srtp2)
	endif()
endif()

if (USE_GNUTLS)
	find_package(GnuTLS REQUIRED)
	if(NOT TARGET GnuTLS::GnuTLS)
		add_library(GnuTLS::GnuTLS UNKNOWN IMPORTED)
		set_target_properties(GnuTLS::GnuTLS PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${GNUTLS_INCLUDE_DIRS}"
			INTERFACE_COMPILE_DEFINITIONS "${GNUTLS_DEFINITIONS}"
			IMPORTED_LINK_INTERFACE_LANGUAGES C
			IMPORTED_LOCATION "${GNUTLS_LIBRARIES}")
	endif()
	target_compile_definitions(datachannel PRIVATE USE_GNUTLS=1)
	target_compile_definitions(datachannel-static PRIVATE USE_GNUTLS=1)
	target_link_libraries(datachannel PRIVATE GnuTLS::GnuTLS)
	target_link_libraries(datachannel-static PRIVATE GnuTLS::GnuTLS)
else()
	find_package(OpenSSL REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_GNUTLS=0)
	target_compile_definitions(datachannel-static PRIVATE USE_GNUTLS=0)
	target_link_libraries(datachannel PRIVATE OpenSSL::SSL)
	target_link_libraries(datachannel-static PRIVATE OpenSSL::SSL)
endif()

if (USE_NICE OR NOT USE_JUICE)
	find_package(LibNice REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_NICE=1)
	target_compile_definitions(datachannel-static PRIVATE USE_NICE=1)
	target_link_libraries(datachannel PRIVATE LibNice::LibNice)
	target_link_libraries(datachannel-static PRIVATE LibNice::LibNice)
else()
	add_subdirectory(deps/libjuice EXCLUDE_FROM_ALL)
	target_compile_definitions(datachannel PRIVATE USE_NICE=0)
	target_compile_definitions(datachannel-static PRIVATE USE_NICE=0)
	target_link_libraries(datachannel PRIVATE LibJuice::LibJuiceStatic)
	target_link_libraries(datachannel-static PRIVATE LibJuice::LibJuiceStatic)
endif()

if(RSA_KEY_BITS_2048)
	target_compile_definitions(datachannel PUBLIC RSA_KEY_BITS_2048)
	target_compile_definitions(datachannel-static PUBLIC RSA_KEY_BITS_2048)
endif()

if(CAPI_STDCALL)
	target_compile_definitions(datachannel PUBLIC CAPI_STDCALL)
	target_compile_definitions(datachannel-static PUBLIC CAPI_STDCALL)
endif()

add_library(LibDataChannel::LibDataChannel ALIAS datachannel)
add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-static)

install(TARGETS datachannel LIBRARY DESTINATION lib)
install(FILES ${LIBDATACHANNEL_HEADERS} DESTINATION include/rtc)

if(NOT MSVC)
	target_compile_options(datachannel PRIVATE -Wall -Wextra)
	target_compile_options(datachannel-static PRIVATE -Wall -Wextra)
endif()

if(WARNINGS_AS_ERRORS)
	if(MSVC)
		target_compile_options(datachannel PRIVATE /WX)
		target_compile_options(datachannel-static PRIVATE /WX)
	else()
		target_compile_options(datachannel PRIVATE -Werror)
		target_compile_options(datachannel-static PRIVATE -Werror)
	endif()
endif()

# Tests
if(NOT NO_TESTS)
	add_executable(datachannel-tests ${TESTS_SOURCES})
	set_target_properties(datachannel-tests PROPERTIES
		VERSION ${PROJECT_VERSION}
		CXX_STANDARD 17)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore") # Prevent a bug in manifest generation for UWP
		set_target_properties(datachannel-tests PROPERTIES OUTPUT_NAME tests)
	endif()
	target_include_directories(datachannel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(datachannel-tests datachannel)

	# Benchmark
	add_executable(datachannel-benchmark test/benchmark.cpp)
	set_target_properties(datachannel-benchmark PROPERTIES
		VERSION ${PROJECT_VERSION}
		CXX_STANDARD 17)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore") # Prevent a bug in manifest generation for UWP
		set_target_properties(datachannel-benchmark PROPERTIES OUTPUT_NAME benchmark)
	endif()
	target_compile_definitions(datachannel-benchmark PRIVATE BENCHMARK_MAIN=1)
	target_include_directories(datachannel-benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(datachannel-benchmark datachannel)
endif()

# Examples
if(NOT NO_EXAMPLES AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	set(JSON_BuildTests OFF CACHE INTERNAL "")
	add_subdirectory(deps/json)
	add_subdirectory(examples/client)
	add_subdirectory(examples/media)
	add_subdirectory(examples/sfu-media)
	add_subdirectory(examples/copy-paste)
	add_subdirectory(examples/copy-paste-capi)
endif()

