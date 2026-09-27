add_library(agentenv-orchestrator STATIC
    service.cc
    persistence.cc
    types.cc
    store.cc
    metrics.cc
    launch_plan.cc
    proxy.cc
)
target_include_directories(agentenv-orchestrator PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-orchestrator PUBLIC
    agentenv-core
    agentenv-sandbox
    agentenv-snapshot
)
