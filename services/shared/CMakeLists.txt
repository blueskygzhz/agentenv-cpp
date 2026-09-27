add_library(agentenv-services-shared STATIC shared.cc)
target_include_directories(agentenv-services-shared
    PUBLIC ${CMAKE_SOURCE_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/include)
