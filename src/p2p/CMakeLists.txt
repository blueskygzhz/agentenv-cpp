add_library(agentenv-p2p STATIC
    types.cc
    error.cc
    config.cc
    discovery.cc
    mock.cc
    iroh.cc
)
target_include_directories(agentenv-p2p PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-p2p PUBLIC agentenv-core)
