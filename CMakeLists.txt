cmake_minimum_required (VERSION 3.7)
project (libdatachannel
	DESCRIPTION "WebRTC Data Channels Library"
	VERSION 0.2.1
	LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

set(LIBDATACHANNEL_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/candidate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/certificate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/channel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/configuration.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/datachannel.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/description.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/dtlstransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/icetransport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/peerconnection.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/rtc.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/sctptransport.cpp
)

set(TESTS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp
)

# Hack because usrsctp uses CMAKE_SOURCE_DIR instead of CMAKE_CURRENT_SOURCE_DIR
set(CMAKE_REQUIRED_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/deps/usrsctp/usrsctplib")

add_subdirectory(deps/usrsctp EXCLUDE_FROM_ALL)

# Set include directory and custom options to make usrsctp compile with recent g++
target_include_directories(usrsctp-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/usrsctp/usrsctplib)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # using regular Clang or AppleClang: Needed since they don't have -Wno-error=format-truncation
  target_compile_options(usrsctp-static PRIVATE -Wno-error=address-of-packed-member)
else()
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
      # GCC version below 9.0 does not support option address-of-packed-member
	  target_compile_options(usrsctp-static PRIVATE -Wno-error=format-truncation)
	else()
	  target_compile_options(usrsctp-static PRIVATE -Wno-error=address-of-packed-member -Wno-error=format-truncation)
	endif()
  else()
    # all other compilers
    target_compile_options(usrsctp-static PRIVATE -Wno-error=address-of-packed-member -Wno-error=format-truncation)
  endif()
endif()

option(USE_GNUTLS "Use GnuTLS instead of OpenSSL" OFF)

find_package(LibNice REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(datachannel SHARED ${LIBDATACHANNEL_SOURCES})
set_target_properties(datachannel PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_include_directories(datachannel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(datachannel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/plog/include)
target_link_libraries(datachannel 
						Threads::Threads
						usrsctp-static 
						LibNice::LibNice						
						)

add_library(datachannel-static STATIC EXCLUDE_FROM_ALL ${LIBDATACHANNEL_SOURCES})
set_target_properties(datachannel-static PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_include_directories(datachannel-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc)
target_include_directories(datachannel-static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(datachannel-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/plog/include)
target_link_libraries(datachannel-static
						Threads::Threads
						usrsctp-static 
						LibNice::LibNice						
						)

if (USE_GNUTLS)
	find_package(GnuTLS REQUIRED)
	if(NOT TARGET GnuTLS::GnuTLS)
		add_library(GnuTLS::GnuTLS UNKNOWN IMPORTED)
		set_target_properties(GnuTLS::GnuTLS PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${GNUTLS_INCLUDE_DIRS}"
			INTERFACE_COMPILE_DEFINITIONS "${GNUTLS_DEFINITIONS}"
			IMPORTED_LINK_INTERFACE_LANGUAGES "C"
			IMPORTED_LOCATION "${GNUTLS_LIBRARIES}")
	endif()
	target_compile_definitions(datachannel PRIVATE USE_GNUTLS=1)
	target_link_libraries(datachannel GnuTLS::GnuTLS)
	target_compile_definitions(datachannel-static PRIVATE USE_GNUTLS=1)
	target_link_libraries(datachannel-static GnuTLS::GnuTLS)
else()
	find_package(OpenSSL REQUIRED)
	target_compile_definitions(datachannel PRIVATE USE_GNUTLS=0)
	target_link_libraries(datachannel OpenSSL::SSL)
	target_compile_definitions(datachannel-static PRIVATE USE_GNUTLS=0)
	target_link_libraries(datachannel-static OpenSSL::SSL)
endif()

add_library(LibDataChannel::LibDataChannel ALIAS datachannel)
add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-static)

add_executable(tests ${TESTS_SOURCES})
set_target_properties(tests PROPERTIES
	VERSION ${PROJECT_VERSION}
	CXX_STANDARD 17)

target_link_libraries(tests datachannel)

