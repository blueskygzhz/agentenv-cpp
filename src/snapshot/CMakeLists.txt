add_library(agentenv-snapshot STATIC
    manager.cc
    layers.cc
    types.cc
    version.cc
    acr_manifest.cc
    drive.cc
    record.cc
    source_registry.cc
    image_export_target.cc
    repository.cc
    artifact_cache.cc
    p2p.cc
    runtime_support.cc
    mock.cc
    repository/errors.cc
    repository/interfaces.cc
    repository/backends.cc
    repository/posixfs/layout.cc
    repository/posixfs/catalog.cc
    # Rust: src/snapshot/repository/backends/posixfs/artifacts.rs
    repository/posixfs/artifacts.cc
    # Rust: src/snapshot/repository/backends/posixfs/runtime.rs
    repository/posixfs/runtime.cc
    # Rust: src/snapshot/repository/backends/posixfs/backend.rs
    repository/posixfs/backend.cc
)
target_include_directories(agentenv-snapshot PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `CommandContext::EffectiveStartCmd` shell-quotes each argument before it
# reaches `bash -lc`.
target_link_libraries(agentenv-snapshot PUBLIC
    agentenv-core
    agentenv-sandbox
    agentenv-p2p
    agentenv-snapshot-startup-pack
    agentenv-shell-util
    # `SnapshotVolume` carries a `VolumeMode`.
    agentenv-volume-record
    # `CommittedSnapshot` carries an `ImageConfigs`.
    agentenv-template-image-configs
    # `snapshot/p2p` keys overlaybd layers by digest/uuid and reads the layer
    # trailer + image config to decide what is publishable.
    agentenv-overlaybd-p2p
    agentenv-overlaybd
)
