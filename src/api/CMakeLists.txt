add_library(agentenv-api STATIC
    dto.cc
    pagination.cc
    server.cc
    proxy.cc
    impls.cc
    # Rust: src/api/impls/attached_drives.rs
    attached_drives.cc
    # Rust: the request-routing half of src/api/proxy.rs
    proxy_route.cc
    # Rust: src/api/impls/auth.rs
    auth.cc
    # Rust: src/api/impls/admin.rs
    admin.cc
)
target_include_directories(agentenv-api PUBLIC ${CMAKE_SOURCE_DIR}/include)
# Note: api depends on orchestrator (via impls.cc). Kept as PUBLIC so downstream
# tests that link -api pick up -orchestrator transitively.
target_link_libraries(agentenv-api PUBLIC
    agentenv-core
    agentenv-orchestrator
    # `attached_drives` validates drive ids / mount paths and resolves each
    # declaration's source image.
    agentenv-sandbox
    agentenv-image
    # `admin` projects observability's NodeSnapshot into the API shapes.
    agentenv-observability-node
)
