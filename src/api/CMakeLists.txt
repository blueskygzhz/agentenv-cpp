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
    # Rust: src/api/impls/volumes.rs
    volumes.cc
    # Rust: src/api/impls/template_helpers.rs
    template_helpers.cc
    # Rust: src/api/impls/sandbox.rs (validation/projection helpers)
    sandbox_params.cc
    # Rust: src/api/impls/image_build.rs (session state + HEALTHCHECK)
    image_build.cc
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
    # `volumes` resolves mount declarations through the VolumeManager.
    agentenv-volume
    # `template_helpers` builds a TemplateBuildSpec from the request DTOs.
    agentenv-template
    # `image_build` shell-quotes the rendered HEALTHCHECK command.
    agentenv-shell-util
)
