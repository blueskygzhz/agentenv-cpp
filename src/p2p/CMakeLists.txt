add_library(agentenv-p2p STATIC
    types.cc
    error.cc
    config.cc
    discovery.cc
    mock.cc
    iroh.cc
    # Rust: src/p2p/mod.rs — the transport/discovery factories. Without these
    # the library cannot be assembled by a caller.
    node.cc
    # Rust: the portable half of src/p2p/iroh/catalog.rs — the published
    # artifact catalog and the catalog wire format. The iroh `ProtocolHandler`
    # that serves it stays in iroh.cc behind AGENTENV_WITH_IROH.
    catalog.cc
    # The HTTP transport: an alternative to iroh for clusters whose peers are
    # directly routable. Deliberately does not link agentenv-api — that would
    # be a cycle (api -> orchestrator -> snapshot -> p2p) — so it declares the
    # serving contract it needs and the caller injects an adapter.
    http_blob_store.cc
    http_transport.cc
    http_routes.cc
    # The scheduler discovery wire over HTTP. Rust uses gRPC here; that path is
    # gated on AGENTENV_WITH_GRPC, which needs protobuf >= 3 and
    # grpc_cpp_plugin. Four calls carrying three strings each do not justify a
    # code generator, and a cluster on the http transport has no gRPC
    # scheduler to talk to anyway.
    scheduler_http.cc
)
target_include_directories(agentenv-p2p PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-p2p PUBLIC
    agentenv-core
    # The published-artifact catalog is persisted, so a restart does not
    # silently retract publications peers are still holding descriptors for.
    agentenv-local-store
)
