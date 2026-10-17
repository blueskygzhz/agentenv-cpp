add_library(agentenv-ublk STATIC
    runner.cc
    ctrl.cc
    dev.cc
    io_buffer.cc
    # Rust: src/queue.rs — the descriptor mapping + FETCH/COMMIT_AND_FETCH
    queue.cc
    # Rust: src/dev.rs — queue_work/slot_task and the thread they run on
    queue_worker.cc
    impls.cc
)
target_include_directories(agentenv-ublk PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-ublk PUBLIC agentenv-core agentenv-storage-util)
if(AGENTENV_WITH_LIBURING)
    target_link_libraries(agentenv-ublk PUBLIC ${URING_LIB})
endif()
