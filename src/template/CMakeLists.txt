add_library(agentenv-template STATIC
    engine.cc
    errors.cc
    build_spec.cc
)
target_include_directories(agentenv-template PUBLIC ${CMAKE_SOURCE_DIR}/include)
# `snapshot` supplies SnapshotAlias / CommandContext / TemplateBuildErrorReason
# and the RepositoryError that TemplatePipelineError wraps.
target_link_libraries(agentenv-template PUBLIC agentenv-core agentenv-snapshot)
