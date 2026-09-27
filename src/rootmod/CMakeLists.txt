add_library(agentenv-rootmod STATIC
    ${CMAKE_SOURCE_DIR}/src/cfg.cc
    ${CMAKE_SOURCE_DIR}/src/privileges.cc
    ${CMAKE_SOURCE_DIR}/src/local_store.cc
    ${CMAKE_SOURCE_DIR}/src/proto.cc
    ${CMAKE_SOURCE_DIR}/src/setup.cc
)
target_include_directories(agentenv-rootmod PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-rootmod PUBLIC agentenv-core agentenv-p2p)
