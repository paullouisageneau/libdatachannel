cmake_minimum_required(VERSION 3.7)

add_executable(datachannel-copy-paste-offerer offerer.cpp)
set_target_properties(datachannel-copy-paste-offerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME offerer)
if(WIN32)
	target_link_libraries(datachannel-copy-paste-offerer datachannel-static) # DLL exports only the C API
else()
	target_link_libraries(datachannel-copy-paste-offerer datachannel)
endif()

add_executable(datachannel-copy-paste-answerer answerer.cpp)
set_target_properties(datachannel-copy-paste-answerer PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME answerer)
if(WIN32)
	target_link_libraries(datachannel-copy-paste-answerer datachannel-static) # DLL exports only the C API
else()
	target_link_libraries(datachannel-copy-paste-answerer datachannel)
endif()

