# `ImageConfigs` serialisation (Rust: src/types/image_configs.rs).
#
# `snapshot::CommittedSnapshot` carries an `ImageConfigs`, while
# `agentenv-template` depends on `agentenv-snapshot` for SnapshotAlias and
# friends. Keeping this one value type in its own library breaks the cycle
# that Rust's single crate never has.
add_library(agentenv-template-image-configs STATIC
    ${CMAKE_SOURCE_DIR}/src/template/image_configs.cc
)
target_include_directories(agentenv-template-image-configs PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-template-image-configs PUBLIC agentenv-core)
