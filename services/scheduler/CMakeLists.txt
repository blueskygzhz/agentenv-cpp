# Rust upstream services/ was Go. In C++11 the placement logic is ported into a
# static library so it can be unit-tested; the daemon binary links against it.
add_library(agentenv-scheduler-internal STATIC
    strategy.cc
    filter.cc
    store.cc
    node_registry.cc
)
target_include_directories(agentenv-scheduler-internal
    PUBLIC ${CMAKE_SOURCE_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-scheduler-internal PUBLIC agentenv-core)

if(AGENTENV_WITH_GRPC)
    message(STATUS "scheduler: AGENTENV_WITH_GRPC enabled but gRPC transport not yet ported")
endif()

add_executable(agentenv-scheduler main.cc)
target_link_libraries(agentenv-scheduler PRIVATE agentenv-scheduler-internal)
