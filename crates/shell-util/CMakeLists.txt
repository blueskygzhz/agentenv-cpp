# shell-util (Rust: crates/shell-util) — a single free function `ShellQuote`,
# depending only on the C++ standard library.
add_library(agentenv-shell-util STATIC shell.cc)
target_include_directories(agentenv-shell-util PUBLIC ${CMAKE_SOURCE_DIR}/include)
