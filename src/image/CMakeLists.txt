add_library(agentenv-image STATIC
    image.cc
    reference.cc
    metadata.cc
    layers.cc
    oci_image.cc
    commit_index.cc
    cache.cc
    # Rust: the `ImageCacheMetadataStore` half of src/image/cache/graph.rs.
    cache_metadata_store.cc
    # Rust: the deleting-GC engine in src/image/cache/service.rs.
    cache_gc.cc
    # Rust: src/image/cache/service.rs (operation-hold lifecycle)
    cache_hold.cc
)
target_include_directories(agentenv-image PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-image PUBLIC
    agentenv-core
    # The cache metadata graph is persisted in a local KV store, keys P2P
    # artifacts by digest, and parses overlaybd image configs to find the
    # `file=` lowers it must pin.
    agentenv-local-store
    agentenv-p2p
    agentenv-overlaybd
)
