cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

add_executable(datachannel-client main.cpp)
set_target_properties(datachannel-client PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client)

if(WIN32)
	target_link_libraries(datachannel-client datachannel-static) # DLL exports only the C API
else()
	target_link_libraries(datachannel-client datachannel)
endif()
target_link_libraries(datachannel-client datachannel nlohmann_json)

