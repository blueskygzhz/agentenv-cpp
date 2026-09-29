add_library(agentenv-observability-node STATIC
    host.cc
    machine.cc
)
target_include_directories(agentenv-observability-node PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-observability-node PUBLIC
    agentenv-core
    Threads::Threads
)
