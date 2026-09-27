add_library(agentenv-linuxcap STATIC cap.cc)
target_include_directories(agentenv-linuxcap PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-linuxcap PUBLIC agentenv-core)
