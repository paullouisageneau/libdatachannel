cmake_minimum_required (VERSION 3.7)
project (libdatachannel
	DESCRIPTION "WebRTC Data Channels Library"
	VERSION 0.2.1
	LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

set(LIBDATACHANNEL_SOURCES
	candidate.cpp
	certificate.cpp
	channel.cpp
	configuration.cpp
	datachannel.cpp
	description.cpp
	dtlstransport.cpp
	icetransport.cpp
	peerconnection.cpp
	rtc.cpp
	sctptransport.cpp
)

list(TRANSFORM LIBDATACHANNEL_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/src/)

# Hack because usrsctp uses CMAKE_SOURCE_DIR instead of CMAKE_CURRENT_SOURCE_DIR
set(CMAKE_REQUIRED_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/usrsctp/usrsctplib")

add_subdirectory(usrsctp EXCLUDE_FROM_ALL)
set(sctp_inet ON)
set(sctp_inet6 ON)

# Set include directory and custom options to make usrsctp compile with recent g++
target_include_directories(usrsctp-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/usrsctp/usrsctplib)
target_compile_options(usrsctp-static PRIVATE -Wno-error=address-of-packed-member -Wno-error=format-truncation)

find_package(GnuTLS REQUIRED)
find_package(LibNice REQUIRED)

if(NOT TARGET GnuTLS::GnuTLS)
    add_library(GnuTLS::GnuTLS UNKNOWN IMPORTED)
    set_target_properties(GnuTLS::GnuTLS PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${GNUTLS_INCLUDE_DIRS}"
        INTERFACE_COMPILE_DEFINITIONS "${GNUTLS_DEFINITIONS}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
		IMPORTED_LOCATION "${GNUTLS_LIBRARIES}")
endif()

add_library(datachannel SHARED ${LIBDATACHANNEL_SOURCES})
set_target_properties(datachannel PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_include_directories(datachannel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel usrsctp-static GnuTLS::GnuTLS LibNice::LibNice)

add_library(datachannel-static STATIC ${LIBDATACHANNEL_SOURCES})
set_target_properties(datachannel-static PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_include_directories(datachannel-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(datachannel-static usrsctp-static GnuTLS::GnuTLS LibNice::LibNice)

add_library(LibDataChannel::LibDataChannel ALIAS datachannel)
add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-static)

