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
)
target_include_directories(agentenv-overlaybd PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-overlaybd PUBLIC agentenv-core agentenv-storage-util)
if(AGENTENV_WITH_ZSTD)
    target_link_libraries(agentenv-overlaybd PUBLIC ${ZSTD_LIB})
endif()
