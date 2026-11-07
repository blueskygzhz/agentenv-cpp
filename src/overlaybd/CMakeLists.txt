# Rust: src/overlaybd/compact.rs — `OverlaybdCompactOutput` + `compact_layers`.
#
# Its own target rather than a member of `agentenv-overlaybd-p2p`: this is the
# publish-compression path, which has nothing to do with the P2P read facade.
add_library(agentenv-overlaybd-compact STATIC
    compact.cc
)
target_include_directories(agentenv-overlaybd-compact PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `agentenv-overlaybd` arrived with `compact_layers`, which needs the LSMT
# merge view, the tar/switch open chain and the zfile compressor. The
# dependency is one-way -- `agentenv-overlaybd` does not know about this
# target -- so it adds no cycle.
target_link_libraries(agentenv-overlaybd-compact PUBLIC agentenv-cfg agentenv-overlaybd)

# Rust: src/overlaybd/ — the P2P HTTP facade for overlaybd layer reads.
add_library(agentenv-overlaybd-p2p STATIC
    p2p/artifact.cc
    p2p/cache.cc
    p2p/facade.cc
    p2p/runtime.cc
)
target_include_directories(agentenv-overlaybd-p2p PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `cfg` for AppConfig-driven facade configuration, `p2p` for the transport.
target_link_libraries(agentenv-overlaybd-p2p PUBLIC
    agentenv-core
    agentenv-p2p
    agentenv-cfg
)
