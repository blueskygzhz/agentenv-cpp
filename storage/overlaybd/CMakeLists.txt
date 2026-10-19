add_library(agentenv-overlaybd STATIC
    # `layer_reader.cc` / `index_tree.cc` are gone: they implemented a
    # `LayerReader` / `IndexTree` / `HeaderTrailer` trio that has no counterpart
    # anywhere in the Rust tree, and whose `HeaderTrailer` and `SegmentMapping`
    # collided by name with the real `lsmt::` ones while describing a different
    # on-disk layout. The real LSMT read path is `lsmt.cc`.
    config.cc
    lsmt.cc
    zfile.cc
    layer.cc
    prefetch.cc
    download_gate.cc
    # Rust: src/{startup_pack,startup_manifest}.rs — the trace and manifest
    # wire formats. Here rather than in agentenv-snapshot because the ublk
    # read path is their other consumer.
    startup_pack.cc
    # Rust: src/io/virtual_file.rs — the abstraction the whole stack layers on
    virtual_file.cc
    # Rust: src/sys/{open_flags,page_cache,release_space,xattr}.rs
    sys.cc
    # Rust: src/image/helper.rs — config path rewriting + runtime upper setup
    image_helper.cc
    # Rust: src/backend/tar.rs — tar detection (the TarFile wrapper is not ported)
    tar.cc
    # Rust: src/backend/local.rs — the first concrete VirtualFile
    local_file.cc
    # Rust: src/lsmt/file/readonly.rs — the merged read view
    lsmt_readonly.cc
    # Rust: src/lsmt/file/readwrite.rs — the writable upper layer
    lsmt_rw.cc
    switch_file.cc
    layer_metadata.cc
    roaring.cc
    cache_meta.cc
    cache_entry.cc
    cache_pool.cc
    cached_fs.cc
    http_transport.cc
    s3_signer.cc
    oss.cc
    pack_planner.cc
    registryfs.cc
)
target_include_directories(agentenv-overlaybd PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-overlaybd PUBLIC agentenv-core agentenv-storage-util)
if(AGENTENV_WITH_ZSTD)
    target_link_libraries(agentenv-overlaybd PUBLIC ${ZSTD_LIB} ${LZ4_LIB})
endif()
if(AGENTENV_WITH_CURL)
    target_link_libraries(agentenv-overlaybd PUBLIC CURL::libcurl)
endif()
