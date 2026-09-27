add_library(agentenv-storage-util STATIC
    types.cc
    aligned_buffer.cc
    id_allocator.cc
    mmap_region.cc
    compact_writer.cc
    io_ring.cc
)
target_include_directories(agentenv-storage-util PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(agentenv-storage-util PUBLIC agentenv-core)
if(AGENTENV_WITH_LIBURING)
    target_link_libraries(agentenv-storage-util PUBLIC ${URING_LIB})
endif()
