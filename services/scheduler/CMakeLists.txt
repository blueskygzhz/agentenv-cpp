# Rust upstream services/ was Go. In C++11 the placement logic is ported into a
# static library so it can be unit-tested; the daemon binary links against it.
add_library(agentenv-scheduler-internal STATIC
    strategy.cc
    filter.cc
    store.cc
    node_registry.cc
    # Go: internal/store.go :: InMemoryArtifactStore — the `key -> nodes`
    # index behind the four P2P discovery calls.
    artifact_index.cc
    # The P2P surface, over HTTP. Upstream serves it on gRPC; that needs a code
    # generator this build does not carry, and the node-side wire in
    # agentenv/p2p/scheduler_http.h already speaks HTTP for the same reason.
    p2p_routes.cc
)
target_include_directories(agentenv-scheduler-internal
    PUBLIC ${CMAKE_SOURCE_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-scheduler-internal PUBLIC
    agentenv-core
    # `p2p_routes` serves on an api::Server. Safe in this direction: the
    # scheduler service is a leaf, so unlike agentenv-p2p it can depend on the
    # HTTP server without closing a cycle.
    agentenv-api)

if(AGENTENV_WITH_GRPC)
    message(STATUS "scheduler: AGENTENV_WITH_GRPC enabled but the gRPC transport is not ported; the P2P surface is served over HTTP")
endif()

add_executable(agentenv-scheduler main.cc)
target_link_libraries(agentenv-scheduler PRIVATE agentenv-scheduler-internal)
