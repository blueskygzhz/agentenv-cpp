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

# `main.rs`'s config resolution and process lifecycle, split from the binary so
# the precedence rules (flag > env > TOML, per field) are testable without
# spawning a daemon.
add_library(agentenv-ublk-daemon-config STATIC
    config.cc
    lifecycle.cc
)
target_include_directories(agentenv-ublk-daemon-config PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-ublk-daemon-config PUBLIC
    agentenv-core
    agentenv-warm-pool
)

# `runtime.rs` — materializing a private overlaybd runtime directory. A library
# of its own because it is the half of the daemon that is testable without a
# ublk device: it only touches the filesystem, overlaybd, and one child process.
add_library(agentenv-ublk-daemon-runtime STATIC
    runtime.cc
)
target_include_directories(agentenv-ublk-daemon-runtime PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-ublk-daemon-runtime PUBLIC
    agentenv-core
    agentenv-overlaybd
    agentenv-ublk-daemon-config
)

add_executable(agentenv-ublk-daemon main.cc)
target_link_libraries(agentenv-ublk-daemon PRIVATE
    agentenv-core
    agentenv-ublk
    agentenv-ublk-daemon-protocol
    agentenv-ublk-daemon-config
    agentenv-ublk-daemon-runtime
)
