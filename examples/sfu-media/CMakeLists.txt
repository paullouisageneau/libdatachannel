cmake_minimum_required(VERSION 3.7)

add_executable(datachannel-sfu-media main.cpp)
set_target_properties(datachannel-sfu-media PROPERTIES
		CXX_STANDARD 17
		OUTPUT_NAME sfu-media)
target_link_libraries(datachannel-sfu-media datachannel nlohmann_json)

