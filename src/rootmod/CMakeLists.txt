add_library(agentenv-rootmod STATIC
    ${CMAKE_SOURCE_DIR}/src/cfg.cc
    ${CMAKE_SOURCE_DIR}/src/privileges.cc
    ${CMAKE_SOURCE_DIR}/src/local_store.cc
    ${CMAKE_SOURCE_DIR}/src/managed_secret.cc
    ${CMAKE_SOURCE_DIR}/src/api_key.cc
    ${CMAKE_SOURCE_DIR}/src/proto.cc
    ${CMAKE_SOURCE_DIR}/src/setup.cc
    ${CMAKE_SOURCE_DIR}/src/setup/kvm.cc
    ${CMAKE_SOURCE_DIR}/src/setup/ublk.cc
    ${CMAKE_SOURCE_DIR}/src/setup/network_capacity.cc
)
target_include_directories(agentenv-rootmod PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `privileges` runs its capability scopes on real threads; `setup/ublk` needs
# the ublk control helpers.
target_link_libraries(agentenv-rootmod PUBLIC
    agentenv-core
    agentenv-p2p
    agentenv-ublk
    Threads::Threads
)
