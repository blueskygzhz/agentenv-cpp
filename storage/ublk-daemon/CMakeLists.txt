# Rust: storage/ublk-daemon
#
# The wire protocol is a library of its own because both sides need it: the
# daemon binary serves it, and the sandbox path speaks it as a client. Keeping
# it inside the executable would force the client to link the daemon.
add_library(agentenv-ublk-daemon-protocol STATIC
    protocol.cc
    client.cc
)
target_include_directories(agentenv-ublk-daemon-protocol PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-ublk-daemon-protocol PUBLIC
    agentenv-core
    agentenv-overlaybd
)

add_executable(agentenv-ublk-daemon main.cc)
target_link_libraries(agentenv-ublk-daemon PRIVATE
    agentenv-core
    agentenv-ublk
    agentenv-ublk-daemon-protocol
)
