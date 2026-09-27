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
