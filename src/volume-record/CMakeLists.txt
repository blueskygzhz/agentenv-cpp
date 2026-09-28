# The volume value types (Rust: src/volume/record.rs).
#
# `snapshot::SnapshotVolume` carries a `VolumeMode`, while `agentenv-volume`
# depends on `agentenv-snapshot` for the repository interface. Rust has one
# crate and no such constraint; splitting the pure value types out is the C++
# cost of that difference.
add_library(agentenv-volume-record STATIC
    ${CMAKE_SOURCE_DIR}/src/volume/record.cc
)
target_include_directories(agentenv-volume-record PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-volume-record PUBLIC agentenv-core)
