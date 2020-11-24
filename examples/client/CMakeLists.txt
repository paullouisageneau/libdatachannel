cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

if(WIN32)
add_executable(datachannel-client main.cpp parse_cl.cpp parse_cl.h getopt.cpp getopt.h)
target_compile_definitions(datachannel-client PUBLIC STATIC_GETOPT)
else()
add_executable(datachannel-client main.cpp parse_cl.cpp parse_cl.h)
endif()

set_target_properties(datachannel-client PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client)
target_link_libraries(datachannel-client datachannel nlohmann_json)

