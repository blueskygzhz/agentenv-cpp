add_library(agentenv-overlaybd STATIC
    layer_reader.cc
    index_tree.cc
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
