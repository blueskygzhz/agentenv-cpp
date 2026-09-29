add_library(agentenv-observability-node STATIC
    host.cc
    machine.cc
    # Rust: src/observability/prometheus.rs — metric label normalization.
    prometheus.cc
    # Rust: src/observability/service.rs — node snapshot projection.
    service.cc
    # Rust: src/observability/reporter.rs — scheduler heartbeat payload + backoff.
    reporter.cc
)
target_include_directories(agentenv-observability-node PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-observability-node PUBLIC
    agentenv-core
    # `prometheus.cc` emits through the crates/observability metrics facade.
    agentenv-observability
    # `reporter.cc` projects orchestrator lifecycle events + cfg onto the wire
    # types; `service.cc` merges OrchestratorMetrics into NodeSnapshot.
    agentenv-cfg
    agentenv-orchestrator
    Threads::Threads
)
