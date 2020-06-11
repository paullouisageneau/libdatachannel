cmake_minimum_required(VERSION 3.7)
project(libdatachannel
	DESCRIPTION "WebRTC DataChannels Library"
	VERSION 0.6.0
	LANGUAGES CXX)

option(USE_GNUTLS "Use GnuTLS instead of OpenSSL" OFF)
option(USE_JUICE "Use libjuice instead of libnice" OFF)
option(RTC_ENABLE_WEBSOCKET "Build WebSocket support" ON)

if(USE_GNUTLS)
	option(USE_NETTLE "Use Nettle instead of OpenSSL in libjuice" ON)
else()
	option(USE_NETTLE "Use Nettle instead of OpenSSL in libjuice" OFF)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
	if (MSVC)
		add_definitions(-DNOMINMAX)
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
	${CMAKE_CURRENT_SOURCE_DIR}/src/peerconnection.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtc.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/sctptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tls.cpp
)

set(LIBDATACHANNEL_WEBSOCKET_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/base64.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tcptransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/tlstransport.cpp
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
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/include.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/init.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/log.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/message.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/peerconnection.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/queue.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/reliability.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.h
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/rtc.hpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/websocket.hpp
)

set(TESTS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/connectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/capi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/benchmark.cpp
)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
find_package(SRTP)

set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)
add_subdirectory(deps/plog)

add_subdirectory(deps/usrsctp EXCLUDE_FROM_ALL)
if (MSYS OR MINGW)
	target_compile_definitions(usrsctp PUBLIC -DSCTP_STDINT_INCLUDE=<stdint.h>)
	target_compile_definitions(usrsctp-static PUBLIC -DSCTP_STDINT_INCLUDE=<stdint.h>)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(usrsctp PRIVATE -Wno-error=format-truncation)
	target_compile_options(usrsctp-static PRIVATE -Wno-error=format-truncation)
endif()
add_library(Usrsctp::Usrsctp ALIAS usrsctp)
add_library(Usrsctp::UsrsctpStatic ALIAS usrsctp-static)

if (RTC_ENABLE_WEBSOCKET)
	add_library(datachannel SHARED
		${LIBDATACHANNEL_SOURCES}
		${LIBDATACHANNEL_WEBSOCKET_SOURCES})
	add_library(datachannel-static STATIC EXCLUDE_FROM_ALL
		${LIBDATACHANNEL_SOURCES}
		${LIBDATACHANNEL_WEBSOCKET_SOURCES})
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=1)
else()
	add_library(datachannel SHARED
		${LIBDATACHANNEL_SOURCES})
	add_library(datachannel-static STATIC EXCLUDE_FROM_ALL
		${LIBDATACHANNEL_SOURCES})
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_WEBSOCKET=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_WEBSOCKET=0)
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
target_link_libraries(datachannel PRIVATE Usrsctp::UsrsctpStatic)

target_include_directories(datachannel-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-static PUBLIC Threads::Threads plog::plog)
target_link_libraries(datachannel-static PRIVATE Usrsctp::UsrsctpStatic)

if(WIN32)
	target_link_libraries(datachannel PRIVATE wsock32 ws2_32) # winsock2
	target_link_libraries(datachannel-static PRIVATE wsock32 ws2_32) # winsock2
endif()

if(SRTP_FOUND)
	if(NOT TARGET SRTP::SRTP)
		add_library(SRTP::SRTP UNKNOWN IMPORTED)
		set_target_properties(SRTP::SRTP PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES ${SRTP_INCLUDE_DIRS}
			IMPORTED_LINK_INTERFACE_LANGUAGES C
			IMPORTED_LOCATION ${SRTP_LIBRARIES})
	endif()
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=1)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=1)
	target_link_libraries(datachannel PRIVATE SRTP::SRTP)
	target_link_libraries(datachannel-static PRIVATE SRTP::SRTP)
else()
	target_compile_definitions(datachannel PUBLIC RTC_ENABLE_MEDIA=0)
	target_compile_definitions(datachannel-static PUBLIC RTC_ENABLE_MEDIA=0)
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

if (USE_JUICE)
	add_subdirectory(deps/libjuice EXCLUDE_FROM_ALL)
	target_compile_definitions(datachannel PRIVATE USE_JUICE=1)
	target_compile_definitions(datachannel-static PRIVATE USE_JUICE=1)
	target_link_libraries(datachannel PRIVATE LibJuice::LibJuiceStatic)
	target_link_libraries(datachannel-static PRIVATE LibJuice::LibJuiceStatic)
else()
	find_package(LibNice REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_JUICE=0)
	target_compile_definitions(datachannel-static PRIVATE USE_JUICE=0)
	target_link_libraries(datachannel PRIVATE LibNice::LibNice)
	target_link_libraries(datachannel-static PRIVATE LibNice::LibNice)
endif()

add_library(LibDataChannel::LibDataChannel ALIAS datachannel)
add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-static)

install(TARGETS datachannel LIBRARY DESTINATION lib)
install(FILES ${LIBDATACHANNEL_HEADERS} DESTINATION include/rtc)

# Tests
add_executable(datachannel-tests ${TESTS_SOURCES})
set_target_properties(datachannel-tests PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)
set_target_properties(datachannel-tests PROPERTIES OUTPUT_NAME tests)
target_include_directories(datachannel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-tests datachannel nlohmann_json::nlohmann_json)

# Benchmark
add_executable(datachannel-benchmark test/benchmark.cpp)
set_target_properties(datachannel-benchmark PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)
set_target_properties(datachannel-benchmark PROPERTIES OUTPUT_NAME benchmark)
target_compile_definitions(datachannel-benchmark PRIVATE BENCHMARK_MAIN=1)
target_include_directories(datachannel-benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-benchmark datachannel)

# Examples
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(deps/json)
add_subdirectory(examples/client)
add_subdirectory(examples/copy-paste)
add_subdirectory(examples/copy-paste-capi)

