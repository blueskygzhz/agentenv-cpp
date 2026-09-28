add_library(agentenv-core STATIC
    identity.cc
    digest.cc
    logging.cc
    executor.cc
    config.cc
    json.cc
    fs.cc
    capability.cc
    process.cc
    credentials.cc
    base64.cc
    file_lock.cc
    dns.cc
    time.cc
    url.cc
    toml.cc
    virtualization.cc
)
target_include_directories(agentenv-core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-core PUBLIC Threads::Threads)
