add_library(agentenv-image STATIC
    image.cc
    reference.cc
    metadata.cc
    layers.cc
    oci_image.cc
    commit_index.cc
    cache.cc
)
target_include_directories(agentenv-image PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-image PUBLIC agentenv-core)
