add_library(agentenv-image STATIC
    # Rust: src/image/mod.rs — the 4xx/5xx classification every API below
    # returns. Listed first because everything else depends on it.
    error.cc
    # Rust: src/image/mod.rs — `PublishedImage`, `IMAGE_TAG_PREFIX`.
    published_image.cc
    # Rust: src/image/catalog.rs
    catalog.cc
    # Rust: src/image/resolver.rs — free functions (referrers, image.json).
    resolver.cc
    reference.cc
    metadata.cc
    local_layer.cc
    layers.cc
    oci_image.cc
    # Rust: the rest of src/image/oci_image.rs -- credentials and regctl, then
    # the layer conversion pipeline.
    oci_registry.cc
    oci_convert.cc
    commit_index.cc
    cache.cc
    # Rust: the `ImageCacheMetadataStore` half of src/image/cache/graph.rs.
    cache_metadata_store.cc
    # Rust: the deleting-GC engine in src/image/cache/service.rs.
    cache_gc.cc
    # Rust: src/image/cache/service.rs (operation-hold lifecycle)
    cache_hold.cc
    # Rust: src/image/buildkit.rs and buildkit/history.rs. The tonic channels
    # are behind the `*Wire` interfaces; buildkit_wire.cc supplies the transport.
    buildkit.cc
    buildkit_history.cc
    buildkit_wire.cc
)
target_include_directories(agentenv-image PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-image PUBLIC
    agentenv-core
    # The cache metadata graph is persisted in a local KV store, keys P2P
    # artifacts by digest, and parses overlaybd image configs to find the
    # `file=` lowers it must pin. `local_layer.cc` also builds a `LayerConfig`.
    agentenv-local-store
    agentenv-p2p
    agentenv-overlaybd
    # `PublishedImage` carries `snapshot::OverlaybdLayerRef` (Rust
    # `crate::types::OverlaybdLayerRef`). The snapshot library does not link
    # back to this one.
    agentenv-snapshot
)
