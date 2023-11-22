cmake_minimum_required(VERSION 3.7)
project(libdatachannel
	VERSION 0.19.4
	LANGUAGES CXX)
set(PROJECT_DESCRIPTION "C/C++ WebRTC network library featuring Data Channels, Media Transport, and WebSockets")

include(GNUInstallDirs)

# Options
option(USE_MBEDTLS "Use Mbed TLS instead of OpenSSL" OFF)
option(USE_GNUTLS "Use GnuTLS instead of OpenSSL" OFF)
option(USE_NICE "Use libnice instead of libjuice" OFF)
option(PREFER_SYSTEM_LIB "Prefer system libraries over deps folder" OFF)
option(USE_SYSTEM_SRTP "Use system libSRTP" ${PREFER_SYSTEM_LIB})
option(USE_SYSTEM_JUICE "Use system libjuice" ${PREFER_SYSTEM_LIB})
option(USE_SYSTEM_USRSCTP "Use system libusrsctp" ${PREFER_SYSTEM_LIB})
option(USE_SYSTEM_PLOG "Use system Plog" ${PREFER_SYSTEM_LIB})
option(USE_SYSTEM_JSON "Use system Nlohmann JSON" ${PREFER_SYSTEM_LIB})
option(NO_WEBSOCKET "Disable WebSocket support" OFF)
option(NO_MEDIA "Disable media transport support" OFF)
option(NO_EXAMPLES "Disable examples" OFF)
option(NO_TESTS "Disable tests build" OFF)
option(WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(CAPI_STDCALL "Set calling convention of C API callbacks stdcall" OFF)
option(SCTP_DEBUG "Enable SCTP debugging output to verbose log" OFF)

if (USE_MBEDTLS AND USE_GNUTLS)
	message(FATAL_ERROR "Both USE_MBEDTLS and USE_GNUTLS can not be enabled at the same time")
endif()


if(USE_GNUTLS)
	option(USE_NETTLE "Use Nettle in libjuice" ON)
else()
	option(USE_NETTLE "Use Nettle in libjuice" OFF)

	if(NOT USE_SYSTEM_SRTP)
		if (USE_MBEDTLS)
			option(ENABLE_MBEDTLS "Enable Mbed TLS crypto engine for libSRTP" ON)
		else()
			option(ENABLE_OPENSSL "Enable OpenSSL crypto engine for libSRTP" ON)
		endif()
	endif()
endif()

if(NOT WARNINGS_AS_ERRORS AND NOT USE_SYSTEM_SRTP)
	option(BUILD_WITH_WARNINGS "Add warning as error flags for libSRTP" OFF)
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF) # to force usrsctp to be built static

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
	if(MSVC)
		add_definitions(-DNOMINMAX)
		add_definitions(-D_CRT_SECURE_NO_WARNINGS)
	endif()
endif()

set(LIBDATACHANNEL_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/candidate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/channel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/configuration.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/datachannel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/description.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/mediahandler.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/global.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/message.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/peerconnection.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtcpreceivingsession.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/track.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/websocket.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/websocketserver.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtppacketizationconfig.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtcpsrreporter.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtppacketizer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/h264rtppacketizer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/nalunit.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/h265rtppacketizer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/h265nalunit.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/av1rtppacketizer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/nalunit.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtcpnackresponder.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtp.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/capi.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/plihandler.cpp
)

set(LIBDATACHANNEL_HEADERS
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/candidate.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/channel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/configuration.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/datachannel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/description.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/mediahandler.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtcpreceivingsession.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/common.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/global.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/message.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/peerconnection.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/reliability.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.h
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtp.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/track.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/websocket.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/websocketserver.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtppacketizationconfig.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtcpsrreporter.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtppacketizer.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/h264rtppacketizer.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/nalunit.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/h265rtppacketizer.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/h265nalunit.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/av1rtppacketizer.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/nalunit.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtcpnackresponder.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/utils.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/plihandler.hpp
)

set(LIBDATACHANNEL_IMPL_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/certificate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/channel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/datachannel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/dtlssrtptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/dtlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/icetransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/init.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/peerconnection.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/logcounter.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/sctptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/threadpool.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tls.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/track.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/utils.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/processor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/sha.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/pollinterrupter.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/pollservice.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/http.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/httpproxytransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tcpserver.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tcptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/transport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/verifiedtlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/websocket.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/websocketserver.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/wstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/wshandshake.cpp
)

set(LIBDATACHANNEL_IMPL_HEADERS
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/certificate.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/channel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/datachannel.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/dtlssrtptransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/dtlstransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/icetransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/init.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/internals.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/peerconnection.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/queue.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/logcounter.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/sctptransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/threadpool.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tls.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/track.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/utils.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/processor.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/sha.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/pollinterrupter.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/pollservice.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/http.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/httpproxytransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tcpserver.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tcptransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/tlstransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/transport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/verifiedtlstransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/websocket.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/websocketserver.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/wstransport.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/impl/wshandshake.hpp
)

set(TESTS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/negotiated.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/reliability.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/turn_connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/websocketserver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi_websocketserver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/benchmark.cpp
)

set(TESTS_UWP_RESOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/Logo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/package.appxManifest
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/SmallLogo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/SmallLogo44x44.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/SplashScreen.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/StoreLogo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/tests/Windows_TemporaryKey.pfx
)

set(BENCHMARK_UWP_RESOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/Logo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/package.appxManifest
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/SmallLogo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/SmallLogo44x44.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/SplashScreen.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/StoreLogo.png
	${CMAKE_CURRENT_SOURCE_DIR}/test/uwp/benchmark/Windows_TemporaryKey.pfx
)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

if(USE_SYSTEM_PLOG)
	find_package(plog REQUIRED)
else()
	set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)
	add_subdirectory(deps/plog EXCLUDE_FROM_ALL)
endif()

if(SCTP_DEBUG)
	add_definitions(-DSCTP_DEBUG)
endif()

if(USE_SYSTEM_USRSCTP)
	find_package(Usrsctp REQUIRED)
else()
	option(sctp_build_shared_lib OFF)
	option(sctp_build_programs OFF)
	option(sctp_inet OFF)
	option(sctp_inet6 OFF)
	option(sctp_werror OFF)
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
	add_subdirectory(deps/usrsctp EXCLUDE_FROM_ALL)
	if (MSYS OR MINGW)
		target_compile_definitions(usrsctp PUBLIC -DSCTP_STDINT_INCLUDE=<stdint.h>)
	endif()
	add_library(Usrsctp::Usrsctp ALIAS usrsctp)
endif()

add_library(datachannel SHARED
	${LIBDATACHANNEL_SOURCES}
	${LIBDATACHANNEL_HEADERS}
	${LIBDATACHANNEL_IMPL_SOURCES}
	${LIBDATACHANNEL_IMPL_HEADERS})
set_target_properties(datachannel PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
	CXX_STANDARD 17
	CXX_VISIBILITY_PRESET default)
target_compile_definitions(datachannel PRIVATE RTC_EXPORTS)

add_library(datachannel-static STATIC EXCLUDE_FROM_ALL
	${LIBDATACHANNEL_SOURCES}
	${LIBDATACHANNEL_HEADERS}
	${LIBDATACHANNEL_IMPL_SOURCES}
	${LIBDATACHANNEL_IMPL_HEADERS})
set_target_properties(datachannel-static PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)
target_compile_definitions(datachannel-static PRIVATE RTC_EXPORTS)
target_compile_definitions(datachannel-static PUBLIC RTC_STATIC)

target_include_directories(datachannel PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel PRIVATE Threads::Threads)
target_link_libraries(datachannel PRIVATE Usrsctp::Usrsctp plog::plog)

target_include_directories(datachannel-static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-static PRIVATE Threads::Threads)
target_link_libraries(datachannel-static PRIVATE Usrsctp::Usrsctp plog::plog)

if(WIN32)
	target_link_libraries(datachannel PUBLIC ws2_32) # winsock2
	target_link_libraries(datachannel-static PUBLIC ws2_32) # winsock2
endif()

if (NO_WEBSOCKET)
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=0)
else()
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=1)
endif()

if(NO_MEDIA)
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=0)
else()
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=1)
	if(USE_SYSTEM_SRTP)
		find_package(libSRTP REQUIRED)
		if(NOT TARGET libSRTP::srtp2)
			add_library(libSRTP::srtp2 UNKNOWN IMPORTED)
			set_target_properties(libSRTP::srtp2 PROPERTIES
				INTERFACE_INCLUDE_DIRECTORIES ${SRTP_INCLUDE_DIRS}
				IMPORTED_LINK_INTERFACE_LANGUAGES C
				IMPORTED_LOCATION ${SRTP_LIBRARIES})
		endif()
		target_compile_definitions(datachannel PRIVATE RTC_SYSTEM_SRTP=1)
		target_compile_definitions(datachannel-static PRIVATE RTC_SYSTEM_SRTP=1)
		target_link_libraries(datachannel PRIVATE libSRTP::srtp2)
		target_link_libraries(datachannel-static PRIVATE libSRTP::srtp2)
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
	if (NOT NO_WEBSOCKET)
		# Needed for SHA1, it should be present as GnuTLS cryptography backend
		find_package(Nettle REQUIRED)
		target_link_libraries(datachannel PRIVATE Nettle::Nettle)
		target_link_libraries(datachannel-static PRIVATE Nettle::Nettle)
	endif()
elseif(USE_MBEDTLS)
	find_package(MbedTLS 3 REQUIRED)

	target_compile_definitions(datachannel PRIVATE USE_MBEDTLS)
	target_compile_definitions(datachannel-static PRIVATE USE_MBEDTLS)
	target_link_libraries(datachannel PRIVATE MbedTLS::MbedTLS)
	target_link_libraries(datachannel-static PRIVATE MbedTLS::MbedTLS)
else()
	if(APPLE)
		# This is a bug in CMake that causes it to prefer the system version over
		# the one in the specified ROOT folder
		if(EXISTS ${OPENSSL_ROOT_DIR})
            # Use static files when OPENSSL_USE_STATIC_LIBS is set.
            # OPENSSL_USE_STATIC_LIBS is what CMake's FindOpenSSL looks at
            # to decide whether to use static libraries.
            if(OPENSSL_USE_STATIC_LIBS)
                set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.a" CACHE FILEPATH "" FORCE)
                set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.a" CACHE FILEPATH "" FORCE)
            else()
                set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib" CACHE FILEPATH "" FORCE)
                set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.dylib" CACHE FILEPATH "" FORCE)
            endif()
		endif()
	endif()
	find_package(OpenSSL REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_GNUTLS=0)
	target_compile_definitions(datachannel-static PRIVATE USE_GNUTLS=0)
	target_link_libraries(datachannel PRIVATE OpenSSL::SSL)
	target_link_libraries(datachannel-static PRIVATE OpenSSL::SSL)
endif()

if (USE_NICE)
	find_package(LibNice REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_NICE=1)
	target_compile_definitions(datachannel-static PRIVATE USE_NICE=1)
	target_link_libraries(datachannel PRIVATE LibNice::LibNice)
	target_link_libraries(datachannel-static PRIVATE LibNice::LibNice)
else()
	target_compile_definitions(datachannel PRIVATE USE_NICE=0)
	target_compile_definitions(datachannel-static PRIVATE USE_NICE=0)
	if(USE_SYSTEM_JUICE)
		find_package(LibJuice REQUIRED)
		target_compile_definitions(datachannel PRIVATE RTC_SYSTEM_JUICE=1)
		target_compile_definitions(datachannel-static PRIVATE RTC_SYSTEM_JUICE=1)
		target_link_libraries(datachannel PRIVATE LibJuice::LibJuice)
		target_link_libraries(datachannel-static PRIVATE LibJuice::LibJuice)
	else()
		add_subdirectory(deps/libjuice EXCLUDE_FROM_ALL)
		target_compile_definitions(datachannel PRIVATE RTC_SYSTEM_JUICE=0)
		target_compile_definitions(datachannel-static PRIVATE RTC_SYSTEM_JUICE=0)
		target_link_libraries(datachannel PRIVATE LibJuice::LibJuiceStatic)
		target_link_libraries(datachannel-static PRIVATE LibJuice::LibJuiceStatic)
	endif()
endif()

if(CAPI_STDCALL)
	target_compile_definitions(datachannel PUBLIC CAPI_STDCALL)
	target_compile_definitions(datachannel-static PUBLIC CAPI_STDCALL)
endif()

set_target_properties(datachannel PROPERTIES EXPORT_NAME LibDataChannel)
add_library(LibDataChannel::LibDataChannel ALIAS datachannel)

set_target_properties(datachannel-static PROPERTIES EXPORT_NAME LibDataChannelStatic)
add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-static)

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

install(TARGETS datachannel EXPORT LibDataChannelTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${LIBDATACHANNEL_HEADERS}
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtc
)

# Export targets
install(
	EXPORT LibDataChannelTargets
	FILE LibDataChannelTargets.cmake
	NAMESPACE LibDataChannel::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LibDataChannel
)

# Export config
install(
	FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LibDataChannelConfig.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LibDataChannel
)

# Export config version
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
	${CMAKE_BINARY_DIR}/LibDataChannelConfigVersion.cmake
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_BINARY_DIR}/LibDataChannelConfigVersion.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LibDataChannel)

# Tests
if(NOT NO_TESTS)
	if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
		# Add resource files needed for UWP apps.
		add_executable(datachannel-tests ${TESTS_SOURCES} ${TESTS_UWP_RESOURCES})
	else()
		add_executable(datachannel-tests ${TESTS_SOURCES})
	endif()

	set_target_properties(datachannel-tests PROPERTIES
		VERSION ${PROJECT_VERSION}
		CXX_STANDARD 17
		OUTPUT_NAME tests)

	set_target_properties(datachannel-tests PROPERTIES
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.tests)

	target_include_directories(datachannel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(datachannel-tests datachannel Threads::Threads)

	# Benchmark
	if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
		# Add resource files needed for UWP apps.
		add_executable(datachannel-benchmark test/benchmark.cpp ${BENCHMARK_UWP_RESOURCES})
	else()
		add_executable(datachannel-benchmark test/benchmark.cpp)
	endif()

	set_target_properties(datachannel-benchmark PROPERTIES
		VERSION ${PROJECT_VERSION}
		CXX_STANDARD 17
		OUTPUT_NAME benchmark)

	set_target_properties(datachannel-benchmark PROPERTIES
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.benchmark)

	target_compile_definitions(datachannel-benchmark PRIVATE BENCHMARK_MAIN=1)
	target_include_directories(datachannel-benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(datachannel-benchmark datachannel Threads::Threads)
endif()

# Examples
if(NOT NO_EXAMPLES)
	set(JSON_BuildTests OFF CACHE INTERNAL "")
	if(USE_SYSTEM_JSON)
		find_package(nlohmann_json REQUIRED)
	else()
		add_subdirectory(deps/json EXCLUDE_FROM_ALL)
	endif()

	if(NOT NO_WEBSOCKET)
		add_subdirectory(examples/client)
		add_subdirectory(examples/client-benchmark)
	endif()
	if(NOT NO_MEDIA)
		add_subdirectory(examples/media-receiver)
		add_subdirectory(examples/media-sender)
		add_subdirectory(examples/media-sfu)
	endif()
	if(NOT NO_MEDIA AND NOT NO_WEBSOCKET)
    	add_subdirectory(examples/streamer)
	endif()
	add_subdirectory(examples/copy-paste)
	add_subdirectory(examples/copy-paste-capi)
endif()

