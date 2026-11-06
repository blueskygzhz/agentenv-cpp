# The recorded runtime versions and their `--version` parsers (Rust:
# src/snapshot/types/version.rs).
#
# `sandbox::ProbeRuntimeVersions` (Rust: src/sandbox/runtime_versions.rs) runs
# the commands and hands the output to these parsers, so it needs them --
# while `agentenv-snapshot` already depends on `agentenv-sandbox` for the
# capture manifest. Linking the whole of `agentenv-snapshot` into the sandbox
# library would close that into a cycle.
#
# Rust has one crate and no such constraint. Splitting the value type and its
# pure parsers out is the C++ cost of that difference, and it is the same shape
# as `agentenv-volume-record` and `agentenv-image-local-layer`: the source file
# does not move, only who links it.
add_library(agentenv-snapshot-version STATIC
    ${CMAKE_SOURCE_DIR}/src/snapshot/version.cc
)
target_include_directories(agentenv-snapshot-version PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-snapshot-version PUBLIC agentenv-core)
