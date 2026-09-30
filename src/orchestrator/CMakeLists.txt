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
    # `FileBackedSandboxPersister` keeps paused-sandbox records in a KV store.
    agentenv-local-store
    agentenv-sandbox
    agentenv-snapshot
    # Rust `volume_manager: Option<Arc<VolumeManager>>` — the delete path
    # publishes / fails / releases volume backings through it.
    agentenv-volume
)
