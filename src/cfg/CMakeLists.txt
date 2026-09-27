add_library(agentenv-cfg STATIC
    cfg.cc
    image.cc
    network.cc
)
target_include_directories(agentenv-cfg PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `cfg/network.cc` reuses `sandbox::Ipv4Cidr` rather than re-implementing CIDR
# parsing, so the sandbox library is a real dependency here.
target_link_libraries(agentenv-cfg PUBLIC agentenv-core agentenv-sandbox agentenv-p2p)
