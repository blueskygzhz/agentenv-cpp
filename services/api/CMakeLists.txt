add_executable(agentenv-api-server main.cc)
target_include_directories(agentenv-api-server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-api-server PRIVATE agentenv-lib)
