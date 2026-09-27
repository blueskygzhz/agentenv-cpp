# AgentENV C++11 — Skeleton

A C++11 architectural port of [kvcache-ai/AgentENV](https://github.com/kvcache-ai/AgentENV).

> This is a **skeleton project** produced as Option B of the port plan:
> - Complete CMake workspace mirroring the Rust workspace crates.
> - Header-first API design: every Rust `trait` / core `struct` is mapped to a C++11
>   abstract class / value type.
> - `.cpp` files stub the bodies with `throw std::runtime_error("not implemented")`
>   so the whole thing compiles and links.
> - `docs/RUST_TO_CPP_MAPPING.md` explains the language-idiom translation table.
>
> **This skeleton is not runtime-functional.** It is a scaffold you can grow
> into a working implementation, module by module.

## Directory layout (mirrors the Rust workspace)

```
agentenv-cpp/
├── CMakeLists.txt                      # top-level workspace
├── README.md
├── docs/
│   ├── ARCHITECTURE.md                 # port strategy + component map
│   └── RUST_TO_CPP_MAPPING.md          # Rust idiom → C++11 idiom table
├── include/agentenv/                   # public C++11 headers (== `src/lib.rs` exports)
│   ├── core/                           # cross-cutting: identity, digest, expected<T,E>
│   ├── api/                            # HTTP DTOs + server trait
│   ├── image/                          # OCI images
│   ├── orchestrator/                   # lifecycle state machine
│   ├── p2p/                            # snapshot P2P (iroh-blobs stand-in)
│   ├── sandbox/                        # sandbox backend trait + Firecracker impl
│   ├── snapshot/                       # snapshot manager + repository
│   └── template/                       # template engine
├── src/                                # implementations
│   ├── core/
│   ├── api/
│   ├── image/
│   ├── orchestrator/
│   ├── p2p/
│   ├── sandbox/
│   ├── snapshot/
│   └── template/
├── crates/                             # sibling crates as CMake sub-projects
│   ├── aenv/                           # CLI
│   ├── linux-cap/
│   ├── shell-util/
│   ├── observability/
│   ├── warm-pool/
│   └── object-store-operator/
├── storage/                            # storage stack
│   ├── util/
│   ├── overlaybd/                      # LSMT read-only format
│   ├── ublk/                           # ublk userspace target trait
│   └── ublk-daemon/                    # daemon skeleton
├── services/                           # control plane (Go-services port)
│   ├── scheduler/                      # gRPC scheduler (C++11 with grpc++)
│   └── gateway/                        # reverse proxy
└── tests/                              # gtest-based tests, one folder per module
```

## Build

Requires: **CMake ≥ 3.16**, a **C++11**-capable compiler (`gcc-4.8+` / `clang-3.4+`).

By default no external dependency is required — every `TODO` interface lives behind
`throw std::runtime_error(...)`.

```bash
mkdir build && cd build
cmake -DAGENTENV_BUILD_TESTS=ON ..
cmake --build . -j
ctest
```

Optional features (turned OFF by default; each enables a real backend):

| CMake option                | Enables                                                     |
|-----------------------------|-------------------------------------------------------------|
| `AGENTENV_WITH_LIBURING`    | ublk / overlaybd real IO (needs liburing ≥ 2.5)             |
| `AGENTENV_WITH_GRPC`        | scheduler / gateway gRPC service                            |
| `AGENTENV_WITH_HTTPLIB`     | HTTP API server (cpp-httplib bundled as single header)      |
| `AGENTENV_WITH_ROCKSDB`     | orchestrator persistent store                               |
| `AGENTENV_WITH_ZSTD`        | overlaybd compression                                       |

## Where to start reading

1. `docs/ARCHITECTURE.md` — port strategy.
2. `docs/RUST_TO_CPP_MAPPING.md` — the idiom translation table (read this before the code).
3. `include/agentenv/core/expected.h` — the `Result<T,E>` equivalent used throughout.
4. `include/agentenv/orchestrator/service.h` — the lifecycle state machine (this is
   the semantic core of AgentENV).
5. `include/agentenv/sandbox/backend.h` — the sandbox backend trait.

## Status matrix

Legend: 🟩 header complete · 🟨 header + stubbed cpp · ⬜ TODO

| Module                        | Header | Stub cpp | Notes                                       |
|-------------------------------|:------:|:--------:|---------------------------------------------|
| core / expected / identity    |  🟩    |   🟨     | `Expected<T,E>` mirrors `Result<T,E>`       |
| core / logging                |  🟩    |   🟨     | ADT-based structured logging               |
| core / digest                 |  🟩    |   🟨     | SHA256 wrapper                              |
| api / DTOs                    |  🟩    |   🟨     | subset of E2B-compatible schema             |
| api / server                  |  🟩    |   🟨     | trait-style handler                         |
| image                         |  🟩    |   🟨     | OCI descriptor / manifest / config          |
| orchestrator / types          |  🟩    |   🟨     | `Sandbox`, `LifecyclePhase` ADT             |
| orchestrator / service        |  🟩    |   🟨     | main state machine                          |
| orchestrator / persistence    |  🟩    |   🟨     | pluggable persister interface               |
| sandbox / backend             |  🟩    |   🟨     | polymorphic base class                      |
| sandbox / firecracker         |  🟩    |   🟨     | Firecracker HTTP client sketch              |
| sandbox / mock                |  🟩    |   🟨     | in-memory fake for tests                    |
| snapshot / manager            |  🟩    |   🟨     | snapshot workflow                           |
| snapshot / repository         |  🟩    |   🟨     | pluggable storage backend                   |
| p2p                           |  🟩    |   🟨     | iroh-blobs stand-in                         |
| storage / util                |  🟩    |   🟨     | offset / range types                        |
| storage / overlaybd           |  🟩    |   🟨     | LSMT read-only header/trailer               |
| storage / ublk                |  🟩    |   🟨     | target trait + io-uring bridge              |
| storage / ublk-daemon         |  🟩    |   🟨     | unix-socket control                         |
| crates / aenv (CLI)           |  🟩    |   🟨     | subcommand framework                        |
| crates / linux-cap            |  🟩    |   🟨     | capability manipulation                     |
| crates / shell-util           |  🟩    |   🟩     | `ShellQuote`, aligned with Rust `shell_quote` |
| crates / observability        |  🟩    |   🟨     | metrics façade                              |
| crates / warm-pool            |  🟩    |   🟩     | header-only `WarmPool<T>`, watermark maintenance |
| services / scheduler          |  🟩    |   🟨     | gRPC ports                                  |
| services / gateway            |  🟩    |   🟨     | HTTP reverse proxy                          |

## License

MIT (matching upstream).
# agentenv-cpp
