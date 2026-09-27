# warm-pool is a header-only generic pool (Rust: crates/warm-pool, WarmPool<T>).
# It carries no translation unit of its own, so expose it as an INTERFACE target
# that only propagates the include path. The name is kept as `agentenv-warm-pool`
# so top-level `agentenv-lib` linkage is unchanged.
add_library(agentenv-warm-pool INTERFACE)
target_include_directories(agentenv-warm-pool INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-warm-pool INTERFACE Threads::Threads)
