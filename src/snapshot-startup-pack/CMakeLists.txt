# The startup-pack descriptor types (Rust: src/snapshot/startup_pack.rs).
#
# `sandbox::SandboxSnapshotManifest` carries a `ResolvedStartupPack`, and
# `agentenv-snapshot` already depends on `agentenv-sandbox`, so the type cannot
# live in either without a cycle. Rust has a single crate and no such
# constraint; this library is the C++ cost of that difference.
add_library(agentenv-snapshot-startup-pack STATIC
    ${CMAKE_SOURCE_DIR}/src/snapshot/startup_pack_types.cc
)
target_include_directories(agentenv-snapshot-startup-pack PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-snapshot-startup-pack PUBLIC agentenv-core)
