add_library(agentenv-snapshot STATIC
    acr_manifest.cc
    image_export_target.cc
    artifact_cache.cc
    p2p.cc
    runtime_support.cc
    # Rust: src/snapshot/resolver.rs — the one resolver that serves every
    # repository. Replaces the posix-only `PosixFsRuntimeResolver`.
    resolver.cc
    # Rust: src/snapshot/recontainerize.rs — prepare a local layer for upload,
    # so the descriptor describes the bytes that actually get uploaded.
    recontainerize.cc
    # Rust: src/snapshot/store.rs — `store_volume_layers`.
    store.cc
    # Rust: src/snapshot/publish.rs — store the files, then commit the record
    publish.cc
    # Rust: the pure helpers of src/snapshot/image_export/acr/{client.rs,
    # source_image.rs} — URL shapes, auth challenges, Docker credentials.
    image_export/acr_parse.cc
    # Rust: src/snapshot/image_export/acr/source_image.rs — read `image.json`
    # and decide, per lower, reuse-remote-blob versus upload-local-delta.
    image_export/acr_source_image.cc
    # Rust: src/snapshot/image_export/acr/publisher.rs — the tag layer, which
    # runs before any blob is uploaded.
    image_export/acr_publisher.cc
    # Rust: src/snapshot/image_export/acr/{client.rs,publisher.rs}, the
    # manifest-delete half: what rolling a publication back needs.
    image_export/acr_client.cc
    # Rust: src/snapshot/image_export/regctl.rs — the `regctl` operations a
    # rootfs export needs, including the cross-registry `blob get | blob put`.
    image_export/regctl.cc
    # Rust: src/snapshot/image_export/service.rs — resolve the target, compare
    # the manifest digest, upload only what the target lacks.
    image_export/service.cc
    repository/errors.cc
    repository/interfaces.cc
    # Rust: src/repository/layout.rs — `DocumentKey`.
    repository/documents.cc
    # Rust: src/repository/layout.rs — `BlobKey`; interfaces.rs blob types.
    repository/blobs.cc
    repository/backends.cc
    repository/posixfs/layout.cc
    repository/posixfs/catalog.cc
    # Rust: src/snapshot/repository/backends/posixfs/artifacts.rs
    repository/posixfs/artifacts.cc
    # Rust: src/snapshot/repository/backends/posixfs/runtime.rs
    # Rust: src/snapshot/repository/backends/posixfs/backend.rs
    repository/posixfs/backend.cc
    # Rust: src/repository/backends/oss/documents.rs — the ETag
    # compare-and-swap that serialises catalog writes across nodes.
    repository/oss/documents.cc
    # Rust: src/repository/backends/oss/lease.rs — the append-only lease that
    # serialises catalog writes on a store with no conditional write.
    repository/oss/lease.cc
    # Rust: src/repository/backends/oss/client.rs — the S3 REST calls the
    # document layer issues, over the overlaybd SigV4 signer and transport.
    repository/oss/client.cc
    # Rust: src/repository/backends/oss/repository.rs — the document and blob
    # halves assembled into a repository.
    repository/oss/repository.cc
    # Rust: src/repository/backends/oss/client.rs `list_keys_recursive`.
    repository/oss/object_store.cc
    # Rust: src/snapshot/catalog.rs — the record layer every backend shares.
    repository/record_catalog.cc
)
target_include_directories(agentenv-snapshot PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `CommandContext::EffectiveStartCmd` shell-quotes each argument before it
# reaches `bash -lc`.
target_link_libraries(agentenv-snapshot PUBLIC
    agentenv-core
    agentenv-sandbox
    # `SnapshotRuntimeVersions` and its `--version` parsers; split out so
    # `agentenv-sandbox` can run the probes that feed them without a cycle.
    agentenv-snapshot-version
    # The record/image-tag value types, in their own target because
    # `agentenv-image` needs exactly those two and this library links it.
    agentenv-snapshot-record
    # `image_export/regctl.cc` shells out through `image::RunRegctl` for the
    # retry budget, GOMAXPROCS cap and 404 classification.
    agentenv-image
    # `StoreDiskLayers` must recognise a runtime-generated rootfs delta:
    # publishing passes `allow_descriptorless = false`, and that delta carries
    # no descriptor, so without this every snapshot of a sandbox that wrote to
    # its rootfs would be refused.
    agentenv-image-local-layer
    agentenv-p2p
    agentenv-snapshot-startup-pack
    agentenv-shell-util
    # `SnapshotVolume` carries a `VolumeMode`.
    agentenv-volume-record
    # `CommittedSnapshot` carries an `ImageConfigs`.
    agentenv-template-image-configs
    # `snapshot/p2p` keys overlaybd layers by digest/uuid and reads the layer
    # trailer + image config to decide what is publishable.
    agentenv-overlaybd-p2p
    # `recontainerize.cc` takes the publish-compression mode as its input.
    agentenv-overlaybd-compact
    agentenv-overlaybd
)
