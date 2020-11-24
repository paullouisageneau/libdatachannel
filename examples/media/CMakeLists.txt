cmake_minimum_required(VERSION 3.7)

add_executable(datachannel-media main.cpp)
set_target_properties(datachannel-media PROPERTIES
        CXX_STANDARD 17
        OUTPUT_NAME media)
target_link_libraries(datachannel-media datachannel nlohmann_json)

