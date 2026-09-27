add_executable(agentenv-ublk-daemon main.cc)
target_link_libraries(agentenv-ublk-daemon PRIVATE
    agentenv-core
    agentenv-ublk
)
