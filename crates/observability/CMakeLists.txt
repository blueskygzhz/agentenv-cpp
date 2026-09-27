add_library(agentenv-observability STATIC
    metrics.cc
    tracing.cc
)
target_include_directories(agentenv-observability PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-observability PUBLIC agentenv-core)
