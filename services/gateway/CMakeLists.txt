add_library(agentenv-gateway-internal STATIC
    host_route.cc
    schedule_hint.cc
)
target_include_directories(agentenv-gateway-internal
    PUBLIC ${CMAKE_SOURCE_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-gateway-internal PUBLIC agentenv-core)

add_executable(agentenv-gateway main.cc)
target_link_libraries(agentenv-gateway PRIVATE agentenv-gateway-internal)
