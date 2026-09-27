add_library(agentenv-object-store-operator STATIC operator.cc)
target_include_directories(agentenv-object-store-operator PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-object-store-operator PUBLIC agentenv-core)
