add_library(agentenv-template STATIC
    engine.cc
    errors.cc
    build_spec.cc
    # Rust: src/template/step_executor.rs
    step_executor.cc
    # Rust: src/template/builder.rs (preparation/validation half)
    builder.cc
    # Rust: src/template/runner.rs (in-sandbox half)
    runner.cc
)
target_include_directories(agentenv-template PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `snapshot` supplies SnapshotAlias / CommandContext / TemplateBuildErrorReason
# and the RepositoryError that TemplatePipelineError wraps.
# `sandbox` supplies the Executor the step executor drives.
# `shell-util` quotes the provisioning script's entity names.
target_link_libraries(agentenv-template PUBLIC agentenv-core agentenv-snapshot agentenv-sandbox agentenv-shell-util)
