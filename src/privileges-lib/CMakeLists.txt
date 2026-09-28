# Capability scoping (Rust: src/privileges.rs).
#
# Split out of `agentenv-rootmod` because `sandbox/network/iptables_util`
# needs `run_with_scoped_capabilities`, and `agentenv-rootmod` already depends
# on `agentenv-sandbox` transitively through `agentenv-cfg`.
add_library(agentenv-privileges STATIC
    ${CMAKE_SOURCE_DIR}/src/privileges.cc
)
target_include_directories(agentenv-privileges PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `run_with_scoped_capabilities` runs its scope on a real thread.
target_link_libraries(agentenv-privileges PUBLIC agentenv-core Threads::Threads)
