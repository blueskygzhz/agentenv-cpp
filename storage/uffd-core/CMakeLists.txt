# Rust: storage/uffd-core
#
# The handler is split from the backend so that a build without overlaybd
# still gets the page-fault machinery: `MemoryImageBackend` is the only thing
# the loop knows about, and the overlaybd image is one implementation of it.
add_library(agentenv-uffd-core STATIC
    uffd.cc
    scm.cc
    backend.cc
    handler.cc
    process_vm_reader.cc
    overlaybd.cc
)
target_include_directories(agentenv-uffd-core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-uffd-core PUBLIC
    agentenv-core
    agentenv-overlaybd
    pthread
)
