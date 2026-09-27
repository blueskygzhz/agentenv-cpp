# `managed_secret` owns the on-disk secret files (Rust: src/managed_secret.rs).
# It lives in its own library because both `agentenv-rootmod` and
# `agentenv-sandbox` need it, and `agentenv-cfg` already depends on
# `agentenv-sandbox` — folding it into either of those would either duplicate
# the objects or create a dependency cycle.
add_library(agentenv-managed-secret STATIC
    ${CMAKE_SOURCE_DIR}/src/managed_secret.cc
)
target_include_directories(agentenv-managed-secret PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-managed-secret PUBLIC agentenv-core)
