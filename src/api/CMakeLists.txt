add_library(agentenv-api STATIC
    dto.cc
    server.cc
    proxy.cc
    impls.cc
)
target_include_directories(agentenv-api PUBLIC ${CMAKE_SOURCE_DIR}/include)
# Note: api depends on orchestrator (via impls.cc). Kept as PUBLIC so downstream
# tests that link -api pick up -orchestrator transitively.
target_link_libraries(agentenv-api PUBLIC agentenv-core agentenv-orchestrator)
