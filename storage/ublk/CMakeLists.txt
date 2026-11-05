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
    # Rust: src/impls/overlaybd_target.rs — the real data plane. `impls.cc`
    # only has the zero-serving placeholder, against the superseded interface.
    overlaybd_target.cc
)
target_include_directories(agentenv-ublk PUBLIC ${CMAKE_SOURCE_DIR}/include)
# agentenv-overlaybd for the image stack the overlaybd target serves. Not a
# cycle: overlaybd links core and storage-util, never ublk.
target_link_libraries(agentenv-ublk PUBLIC agentenv-core agentenv-storage-util
    agentenv-overlaybd)
if(AGENTENV_WITH_LIBURING)
    target_link_libraries(agentenv-ublk PUBLIC ${URING_LIB})
endif()
