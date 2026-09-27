add_library(agentenv-uffd-core STATIC uffd.cc)
target_include_directories(agentenv-uffd-core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-uffd-core PUBLIC agentenv-core)
