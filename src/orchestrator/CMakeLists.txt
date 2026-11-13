add_library(agentenv-orchestrator STATIC
    service.cc
    persistence.cc
    types.cc
    store.cc
    metrics.cc
    # Rust: src/orchestrator/sandbox_metrics.rs (cache + concurrency rule; the
    # sampling task itself needs SandboxBackend::Envd)
    sandbox_metrics.cc
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
    # Rust `image_refs: Arc<dyn RuntimeImageRefs>` — `New` builds the port
    # with `local_image_services_from_app_config` and the maintenance sweep
    # calls `MaintainRunning` / `ReconcilePaused` through it. Reached
    # transitively via agentenv-snapshot today; named here because this
    # library now depends on it directly.
    agentenv-image
    # Rust `volume_manager: Option<Arc<VolumeManager>>` — the delete path
    # publishes / fails / releases volume backings through it.
    agentenv-volume
)
