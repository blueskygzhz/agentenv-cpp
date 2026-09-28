add_library(agentenv-snapshot STATIC
    manager.cc
    layers.cc
    types.cc
    version.cc
    acr_manifest.cc
    repository.cc
    artifact_cache.cc
    p2p.cc
    runtime_support.cc
    mock.cc
    repository/errors.cc
    repository/interfaces.cc
    repository/backends.cc
    repository/posixfs/layout.cc
)
target_include_directories(agentenv-snapshot PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-snapshot PUBLIC agentenv-core agentenv-sandbox agentenv-p2p agentenv-snapshot-startup-pack)
