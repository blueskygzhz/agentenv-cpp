add_library(agentenv-volume STATIC
    record.cc
    volume.cc
)
target_include_directories(agentenv-volume PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `snapshot` brings the repository interface and the layer descriptors;
# `overlaybd` brings ImageConfig for the local image.json manipulation.
target_link_libraries(agentenv-volume PUBLIC
    agentenv-core
    agentenv-snapshot
    agentenv-overlaybd
)
