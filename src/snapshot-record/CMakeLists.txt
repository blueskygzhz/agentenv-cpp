# The snapshot record value types and the snapshot image-tag helpers (Rust:
# src/snapshot/types/record.rs, src/snapshot/types/image_tag.rs).
#
# Split out to break a cycle in the *other* direction from
# `agentenv-image-local-layer`. `agentenv-image` needs only these two
# translation units out of `agentenv-snapshot` -- `catalog.cc` reads
# `snapshot/record.h` and `resolver.cc` reads `snapshot/image_tag.h` -- while
# `agentenv-snapshot` needs the regctl primitives from `agentenv-image` for the
# rootfs image export. Linking both whole libraries to each other is the cycle;
# this is the smallest cut that removes it.
#
# Both files depend on `agentenv-core` alone, which is what makes the cut
# viable at all. As with the other fine-grained targets, the sources do not
# move -- only who links them.
add_library(agentenv-snapshot-record STATIC
    ${CMAKE_SOURCE_DIR}/src/snapshot/record.cc
    ${CMAKE_SOURCE_DIR}/src/snapshot/image_tag.cc
    # `record.cc` serialises the types these define -- `OverlaybdLayerRef`,
    # `ManagedLayer`, `CommittedAttachedDrive` -- so the cut has to include
    # them or it is not a self-contained library.
    ${CMAKE_SOURCE_DIR}/src/snapshot/types.cc
    ${CMAKE_SOURCE_DIR}/src/snapshot/drive.cc
    # The layer-ref JSON codec, which `record.cc` and `drive.cc` both use.
    ${CMAKE_SOURCE_DIR}/src/snapshot/layers.cc
)
target_include_directories(agentenv-snapshot-record PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-snapshot-record PUBLIC
    agentenv-core
    # `CommittedSnapshot` carries the per-layer OCI image configs, so decoding
    # a record needs `tpl::ImageConfigs`. Already its own target for the same
    # reason: several libraries need the value type and nothing else of
    # `agentenv-template`.
    agentenv-template-image-configs
    # `types.cc` shell-quotes the overlaybd device paths it formats.
    agentenv-shell-util
)
