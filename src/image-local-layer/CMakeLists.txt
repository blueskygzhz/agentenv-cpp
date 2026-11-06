# The local-layer predicates and `LayerConfig` builder (Rust:
# src/image/local_layer.rs).
#
# `snapshot::StoreDiskLayers` must ask whether a descriptorless lower is a
# runtime-generated rootfs delta — publishing passes
# `allow_descriptorless = false`, and a sandbox's writable rootfs delta carries
# no descriptor, so without that question every snapshot of a sandbox that
# wrote to its rootfs would be refused. Meanwhile `agentenv-image` depends on
# `agentenv-snapshot` for `OverlaybdLayerRef`. Rust has one crate and no such
# constraint; splitting these pure predicates out is the C++ cost of that
# difference.
add_library(agentenv-image-local-layer STATIC
    ${CMAKE_SOURCE_DIR}/src/image/local_layer.cc
)
target_include_directories(agentenv-image-local-layer PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-image-local-layer PUBLIC
    agentenv-core
    # `LayerConfigFromLocalLayer` returns a `storage::overlaybd::LayerConfig`.
    agentenv-overlaybd
)
