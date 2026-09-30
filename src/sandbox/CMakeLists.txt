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
    # firecracker sub-module (Rust: src/sandbox/firecracker/*.rs)
    firecracker/config.cc
    # Rust: src/sandbox/firecracker/overlaybd_snapshot.rs (dirty-page mapping)
    firecracker/dirty_memory.cc
    # Rust: src/sandbox/firecracker/process_vm_reader.rs
    firecracker/process_vm.cc
    # Rust: src/sandbox/firecracker/{pool,startup_pack,config}.rs
    firecracker/lifecycle.cc
    firecracker/socket.cc
    firecracker/instance.cc
    firecracker/mmds.cc
 firecracker/pool.cc
    firecracker/sandbox.cc
)
target_include_directories(agentenv-sandbox PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-sandbox PUBLIC
    agentenv-core
    agentenv-cfg
    agentenv-managed-secret
    agentenv-snapshot-startup-pack
    agentenv-privileges

)
