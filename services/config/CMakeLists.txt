add_library(agentenv-services-config STATIC store.cc)
target_include_directories(agentenv-services-config
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(agentenv-services-config PUBLIC agentenv-core)
