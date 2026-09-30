add_library(agentenv-p2p STATIC
    types.cc
    error.cc
    config.cc
    discovery.cc
    mock.cc
    iroh.cc
    # Rust: the portable half of src/p2p/iroh/catalog.rs — the published
    # artifact catalog and the catalog wire format. The iroh `ProtocolHandler`
    # that serves it stays in iroh.cc behind AGENTENV_WITH_IROH.
    catalog.cc
)
target_include_directories(agentenv-p2p PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-p2p PUBLIC
    agentenv-core
    # The published-artifact catalog is persisted, so a restart does not
    # silently retract publications peers are still holding descriptors for.
    agentenv-local-store
)
