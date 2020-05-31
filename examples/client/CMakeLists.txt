cmake_minimum_required(VERSION 3.7)
cmake_policy(SET CMP0079 NEW)

add_executable(datachannel-client main.cpp)
set_target_properties(datachannel-client PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME client)
target_link_libraries(datachannel-client datachannel nlohmann_json)

