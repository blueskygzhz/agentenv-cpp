# Rust: src/local_store.rs
#
# Its own target because three unrelated layers need the KV store: `rootmod`
# (setup/config), `orchestrator` (paused-sandbox records) and `image` (cache
# metadata graph). Folding it into any one of them would drag that layer's
# dependencies into the other two.
add_library(agentenv-local-store STATIC
    ${CMAKE_SOURCE_DIR}/src/local_store.cc
)
target_include_directories(agentenv-local-store PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-local-store PUBLIC agentenv-core)
