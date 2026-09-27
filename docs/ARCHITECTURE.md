# Architecture

This document explains how the AgentENV Rust workspace maps onto this C++11 skeleton.

## 0. High-level component map

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Client (aenv CLI, or user's SDK)                                        │
│    ┌────────────────────────────────┐                                    │
│    │  crates/aenv (C++11)           │                                    │
│    └────────────────────────────────┘                                    │
│                       │ HTTP/JSON (E2B-compatible)                       │
└───────────────────────┼─────────────────────────────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  agentenv (single node control-plane)   —— C++11 library + binary        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  src/api    : HTTP server, E2B & Custom Extension DTO layer       │  │
│  │  src/orchestrator : lifecycle state machine over sandboxes        │  │
│  │  src/sandbox: pluggable backend (Firecracker microVM, Mock)       │  │
│  │  src/snapshot: OCI image import, layer cache, P2P                 │  │
│  │  src/template: prebuilt image compose                             │  │
│  │  src/image  : OCI descriptor / manifest / config                  │  │
│  │  src/p2p    : peer-to-peer transfer (iroh-blobs stand-in)         │  │
│  │  core       : cross-cutting (identity, Expected, logging, ...)    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                       │                                                  │
│                       │ ublk uring / firecracker HTTP                    │
└───────────────────────┼─────────────────────────────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  Storage stack  ── C++11 crates                                          │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  storage/util       : offset / range / bytes                      │  │
│  │  storage/overlaybd  : LSMT read-only format (header, trailer, ...)│  │
│  │  storage/ublk       : userspace ublk target trait + io_uring pump │  │
│  │  storage/ublk-daemon: unix-socket control agent                   │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                       │                                                  │
└───────────────────────┼─────────────────────────────────────────────────┘
                        ▼
                        Kernel: ublk driver, KVM (via Firecracker)
```

## 1. Rust workspace → CMake targets

Every `Cargo.toml` member becomes a `add_library(...)` or `add_executable(...)` in
CMake. The exact mapping:

| Rust workspace member                     | CMake target                          | Kind    |
|-------------------------------------------|---------------------------------------|---------|
| `.` (top-level lib)                       | `agentenv-lib`                        | STATIC  |
| `.` (top-level bin)                       | `agentenv`                            | EXE     |
| `adev`                                    | `agentenv-adev`                       | EXE     |
| `crates/aenv`                             | `agentenv-aenv`                       | EXE     |
| `crates/linux-cap`                        | `agentenv-linuxcap`                   | STATIC  |
| `crates/observability`                    | `agentenv-observability`              | STATIC  |
| `crates/object-store-operator`            | `agentenv-object-store-operator`      | STATIC  |
| `crates/shell-util`                       | `agentenv-shell-util`                 | STATIC  |
| `crates/warm-pool`                        | `agentenv-warm-pool`                  | STATIC  |
| `crates/benchmarks`                       | `agentenv-benchmarks`                 | EXE     |
| `crates/e2e-tests`                        | `agentenv-e2e-tests`                  | EXE     |
| `crates/test-support`                     | `agentenv-test-support`               | STATIC  |
| `thirdparty/firecracker-client`           | `agentenv-thirdparty-firecracker`     | STATIC  |
| `thirdparty/envd`                         | `agentenv-thirdparty-envd`            | STATIC  |
| `src/api/generated`                       | `agentenv-http-server`                | STATIC  |
| `src/custom_extension_api/generated`      | `agentenv-custom-extension`           | STATIC  |
| `storage/util`                            | `agentenv-storage-util`               | STATIC  |
| `storage/overlaybd`                       | `agentenv-overlaybd`                  | STATIC  |
| `storage/ublk`                            | `agentenv-ublk`                       | STATIC  |
| `storage/ublk-daemon`                     | `agentenv-ublk-daemon`                | EXE     |
| `services/scheduler` (Go)                 | `agentenv-scheduler` (C++11 grpc)     | EXE     |
| `services/gateway` (Go)                   | `agentenv-gateway` (C++11 httplib)    | EXE     |

## 2. Lifecycle state machine (the semantic core)

`src/orchestrator/service.rs` is the semantic center of AgentENV. Its state machine:

```
                              ┌────────────┐
                              │  Created   │
                              └────┬───────┘
                                   │ allocate resources
                                   ▼
                              ┌────────────┐
                              │  Reserved  │
                              └────┬───────┘
                                   │ boot backend (Firecracker + envd)
                                   ▼
                              ┌────────────┐
                              │  Booting   │
                              └────┬───────┘
                        healthy    │      timeout / crash
                    ┌──────────────┼───────────────┐
                    ▼              ▼               ▼
              ┌──────────┐   ┌───────────┐   ┌───────────┐
              │  Ready   │   │ Snapshot  │   │  Failed   │
              └────┬─────┘   └────┬──────┘   └───────────┘
                   │              │ restore
                   ▼              ▼
              ┌──────────┐   ┌───────────┐
              │  Running │   │  Booting  │
              └────┬─────┘   └───────────┘
                   │ pause / snapshot / stop
                   ▼
              ┌──────────┐
              │ Stopped  │
              └──────────┘
```

C++11 impl: `include/agentenv/orchestrator/service.h` declares
`orchestrator::Service` and `LifecyclePhase`. The state transitions are the
member functions `Create / Reserve / Boot / MarkReady / Snapshot / Stop`.

## 3. Backend polymorphism

Rust:
```rust
#[async_trait]
pub trait Backend: Send + Sync {
    async fn boot(&self, plan: LaunchPlan) -> Result<Handle, Error>;
    ...
}
```

C++11:
```cpp
class Backend {
 public:
    virtual ~Backend() = default;
    virtual std::future<Expected<Handle, Error>> Boot(LaunchPlan plan) = 0;
    ...
};
class FirecrackerBackend final : public Backend { ... };
class MockBackend         final : public Backend { ... };
```

`orchestrator::Service` owns a `std::shared_ptr<Backend>`; test code injects a
`MockBackend`.

## 4. HTTP layer

Rust uses `axum + tower + hyper` with codegen from `openapi.yaml`.

C++11 port strategy:
- **Do NOT** try to port `axum`. Instead:
  - **`AGENTENV_WITH_HTTPLIB`** (default OFF): use bundled `cpp-httplib` (single
    header, no deps). Great for dev / demo.
  - **`AGENTENV_WITH_ASIO`**: use `boost::beast` (or standalone Asio+Beast).
  - **Custom**: an `api::Server` trait with `Handle(Request&, Response*)`.
- Every DTO struct in `include/agentenv/api/dto/*.h` has hand-written
  `to_json / from_json`. That's what upstream generates from `openapi.yaml`.

## 5. Snapshot pipeline

Rust: `src/snapshot/` uses `iroh-blobs` for P2P, `opendal` for object store,
`overlaybd` for the local block format.

C++11: kept modular but simpler:
- `snapshot::Manager` owns a `snapshot::Repository*` (interface).
- Repositories: `LocalRepository` (files under a dir), `S3Repository` (via
  libcurl), `MockRepository` (in-memory).
- P2P layer (`p2p::Node`) is a **stubbed interface**; you can plug a real
  transport later.

## 6. Storage stack

The most delicate port. Kept as **read-only skeleton** for now:

- `storage/overlaybd/`: LSMT read-only file format decoder. `HeaderTrailer`,
  `IndexBlock`, `SegmentAddress`. Real IO is `pread`; `AGENTENV_WITH_LIBURING`
  enables the io_uring path.
- `storage/ublk/`: `Target` interface + a control plane skeleton. The real
  ublk kernel interaction is guarded behind `AGENTENV_WITH_UBLK` (Linux ≥ 6.8).
  For the skeleton, everything compiles as a **CPU-only mock target**.

## 7. Threading model

- Global `core::Executor` (thread pool, N = hw threads by default).
- One dedicated **IO thread** per open `core::IoRing` (io_uring reactor).
- HTTP server uses its own thread pool internally.
- gRPC scheduler / gateway use `grpc++`'s completion queue threads.

No global runtime; each subsystem owns its threads and joins them on
destructor.

## 8. Configuration

Rust: `confique + toml`.
C++11: `agentenv::core::Config` — a hand-written TOML subset parser producing
strongly-typed structs (`OrchestratorConfig`, `SandboxConfig`, ...).

## 9. What is deliberately NOT ported in this skeleton

- **iroh-blobs P2P**: interface only, no real implementation. iroh is a
  complex QUIC-based P2P stack; a real port is > 1 person-year.
- **userfaultfd snapshots** (`storage/uffd-core`): stubbed. Requires kernel
  UFFD deeply.
- **Firecracker HTTP client**: sketched only (needs OpenAPI codegen or a
  hand-written subset).
- **envd (guest agent)**: header-only, no build.
- **prost / tonic-generated gRPC**: guarded behind `AGENTENV_WITH_GRPC`,
  with `.proto` files copied from upstream and generated via `protoc`.

## 10. Growth roadmap

1. Fill in `core/` — you need `Expected`, `Executor`, `IoRing`, `MpscQueue`.
2. Fill in `orchestrator::Service` — the state machine can be implemented
   without any real backend, using `MockBackend`.
3. Wire `api::Server` to the orchestrator; you now have an HTTP-level demo
   with fake sandboxes.
4. Add a real `sandbox::firecracker::Backend` (needs Firecracker HTTP client).
5. Add `snapshot::LocalRepository`; you can now cache OCI images.
6. Add `storage/overlaybd` real decode; use `pread` first, io_uring later.
7. Add `storage/ublk`; register a target with the kernel driver.
8. Add `services/scheduler` gRPC (multi-node).

Each of those is a bounded, testable slice.
