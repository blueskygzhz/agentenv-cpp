cmake_minimum_required(VERSION 3.16)
project(agentenv_cpp
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "C++11 port skeleton of kvcache-ai/AgentENV"
)

# ---- language ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- warnings ------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-missing-field-initializers
    )
endif()

# ---- options -------------------------------------------------------------
option(AGENTENV_BUILD_TESTS      "Build unit tests (needs GoogleTest)" ON)
option(AGENTENV_WITH_LIBURING    "Enable io_uring backend"            OFF)
option(AGENTENV_WITH_GRPC        "Build gRPC scheduler/gateway"       OFF)
option(AGENTENV_WITH_HTTPLIB     "Use cpp-httplib for HTTP server"    OFF)
option(AGENTENV_WITH_ROCKSDB     "Use RocksDB for orchestrator store" OFF)
option(AGENTENV_WITH_ZSTD        "Enable zstd in overlaybd"           OFF)

# ---- output layout -------------------------------------------------------
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- pthread -------------------------------------------------------------
find_package(Threads REQUIRED)

# ---- global include path -------------------------------------------------
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- optional dependencies ----------------------------------------------
if(AGENTENV_WITH_LIBURING)
    find_library(URING_LIB uring REQUIRED)
    add_compile_definitions(AGENTENV_WITH_LIBURING=1)
endif()

if(AGENTENV_WITH_HTTPLIB)
    add_compile_definitions(AGENTENV_WITH_HTTPLIB=1)
endif()

if(AGENTENV_WITH_ROCKSDB)
    find_package(RocksDB REQUIRED)
    add_compile_definitions(AGENTENV_WITH_ROCKSDB=1)
endif()

if(AGENTENV_WITH_GRPC)
    find_package(gRPC REQUIRED)
    find_package(Protobuf REQUIRED)
    add_compile_definitions(AGENTENV_WITH_GRPC=1)
endif()

if(AGENTENV_WITH_ZSTD)
    find_library(ZSTD_LIB zstd REQUIRED)
    add_compile_definitions(AGENTENV_WITH_ZSTD=1)
endif()

# ---- subdirectories: order matters (deps go first) ----------------------
# Rust `src/lib.rs` module order (bottom-up):
#   core → image → p2p → template → snapshot → sandbox → orchestrator → api
add_subdirectory(src/core)
# `snapshot::CommandContext` shell-quotes start commands, so this must be
# configured before the modules that link it.
add_subdirectory(crates/shell-util)
add_subdirectory(src/template-image-configs)
add_subdirectory(src/volume-record)
add_subdirectory(src/managed-secret)
add_subdirectory(src/privileges-lib)
add_subdirectory(src/snapshot-startup-pack)
add_subdirectory(src/image)
add_subdirectory(src/p2p)
add_subdirectory(src/overlaybd)
add_subdirectory(src/template)
add_subdirectory(src/snapshot)
add_subdirectory(src/sandbox)
add_subdirectory(src/orchestrator)
add_subdirectory(src/api)

# Rust `src/cfg.rs` + `src/cfg/{image,network}.rs`. Depends on sandbox for
# `Ipv4Cidr` and on overlaybd for `UpperMode`, so it is configured after them.
add_subdirectory(src/cfg)

# Rust `src/volume.rs`. Depends on the snapshot repository interface and on
# overlaybd's ImageConfig.
add_subdirectory(src/volume)

# Rust: crate-root modules that live directly under `src/`.
add_subdirectory(src/rootmod)

add_subdirectory(storage/util)
add_subdirectory(storage/overlaybd)
add_subdirectory(storage/ublk)
add_subdirectory(storage/ublk-daemon)
add_subdirectory(storage/uffd-core)

add_subdirectory(crates/linux-cap)
add_subdirectory(crates/observability)
add_subdirectory(crates/object-store-operator)
add_subdirectory(crates/warm-pool)
add_subdirectory(crates/aenv)

add_subdirectory(services/scheduler)
add_subdirectory(services/gateway)
add_subdirectory(services/api)
add_subdirectory(services/config)
add_subdirectory(services/shared)

# ---- top-level agentenv library aggregating everything -----------------
add_library(agentenv-lib INTERFACE)
target_link_libraries(agentenv-lib INTERFACE
    agentenv-core
    agentenv-image
    agentenv-p2p
    agentenv-template
    agentenv-snapshot
    agentenv-sandbox
    agentenv-orchestrator
    agentenv-api
    agentenv-cfg
    agentenv-rootmod
    agentenv-storage-util
    agentenv-overlaybd
    agentenv-ublk
    agentenv-uffd-core
    agentenv-linuxcap
    agentenv-shell-util
    agentenv-observability
    agentenv-object-store-operator
    agentenv-warm-pool
    agentenv-services-config
    agentenv-services-shared
    Threads::Threads
)

# ---- top-level main binary ---------------------------------------------
add_executable(agentenv src/main.cc)
target_link_libraries(agentenv PRIVATE agentenv-lib)

# ---- tests --------------------------------------------------------------
if(AGENTENV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- install -----------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS agentenv DESTINATION ${CMAKE_INSTALL_BINDIR})
