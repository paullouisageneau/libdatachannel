cmake_minimum_required(VERSION 3.7)

add_executable(datachannel-copy-paste-offerer offerer.cpp)
set_target_properties(datachannel-copy-paste-offerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME offerer)
target_link_libraries(datachannel-copy-paste-offerer datachannel)

add_executable(datachannel-copy-paste-answerer answerer.cpp)
set_target_properties(datachannel-copy-paste-answerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME answerer)
target_link_libraries(datachannel-copy-paste-answerer datachannel)

