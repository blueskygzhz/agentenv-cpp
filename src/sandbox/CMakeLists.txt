add_library(agentenv-sandbox STATIC
    # top-level sandbox modules (Rust: src/sandbox/*.rs)
    backend.cc
    mock.cc
    envd.cc
    extra_drive.cc
    process.cc
    network.cc
    access.cc
    manifest.cc
    network/iptables_util.cc
    network/policy.cc
    network/slot.cc
    network/manager.cc
    # Rust: src/sandbox/network/resolver.rs — host-namespace DNS worker.
    network/resolver.cc
    # Rust: src/sandbox/network/egress_proxy.rs — preface inspection + policy staging.
    network/egress_proxy.cc
    custom_extension.cc
    ublk.cc
    # Rust: sandbox/ublk/device.rs :: UblkDeviceManager + SharedReadOnlyDevice
    ublk/device.cc
    # firecracker sub-module (Rust: src/sandbox/firecracker/*.rs)
    firecracker/config.cc
    # Rust: the Serialize/Deserialize derives on the firecracker configs —
    # this is the on-disk form of a paused sandbox.
    firecracker/config_json.cc
    # Rust: src/sandbox/firecracker/overlaybd_snapshot.rs (dirty-page mapping)
    firecracker/dirty_memory.cc
    # Rust: src/sandbox/firecracker/process_vm_reader.rs
    firecracker/process_vm.cc
    # Rust: src/sandbox/firecracker/{pool,startup_pack,config}.rs
    firecracker/lifecycle.cc
    firecracker/socket.cc
    firecracker/instance.cc
    # Rust: the firecracker_client::models bodies instance.rs sends
    firecracker/instance_api.cc
    # Rust: sandbox.rs :: configure_microvm — the pre-boot call sequence
    firecracker/configure.cc
    # Rust: impl SandboxBackend for FirecrackerSandbox
    firecracker/sandbox_backend.cc
    # Rust: overlaybd_snapshot.rs :: build_mem_snapshot_image_config
    firecracker/mem_image_config.cc
    # Rust: factory.rs :: impl SandboxBackendFactory
    firecracker/factory_impl.cc
    firecracker/mmds.cc
 firecracker/pool.cc
    firecracker/sandbox.cc
    # Rust: src/sandbox/firecracker/factory.rs (config half of `build`)
    firecracker/factory.cc
    # Rust: firecracker/sandbox.rs :: FirecrackerPausedState
    firecracker/paused_state.cc
)
target_include_directories(agentenv-sandbox PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-sandbox PUBLIC
    agentenv-core
    agentenv-cfg
    agentenv-managed-secret
    agentenv-snapshot-startup-pack
    agentenv-privileges
    # `ublk::OverlaybdDeviceConfig` serializes `runtime_upper_mode` through
    # `storage::overlaybd::UpperModeToString` / `UpperModeParse`, so the
    # persisted spelling has exactly one definition.
    agentenv-overlaybd
    # sandbox/ublk/device.cc drives the daemon over its wire protocol.
    agentenv-ublk-daemon-protocol
)
