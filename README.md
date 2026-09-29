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

Legend: 🟩 behaviourally aligned + tested · 🟨 main path real, some branches missing · 🟧 skeleton / types only · ⬜ not started

### Recently brought to strict alignment

| Module | Rust source | Notes |
|---|---|---|
| `p2p` interface layer | `src/p2p/{error,types,transport,mock,discovery}.rs` | 5-variant error set with verbatim `thiserror` strings; `P2pFetchOptions` (default `advertise=true`); the trait's provided methods (`lookup`/`fetch`/`fetch_bytes`/`shutdown`); `DisabledP2pTransport`; mock `unpublish` bookkeeping + delay injection; `peers_for_key` now issues a keyed lookup RPC instead of serving the refresh cache |
| `observability/prometheus` | `src/observability/prometheus.rs` | route/method/status label normalization (bounded cardinality), `MetricGuard` with drop⇒`canceled`, `SandboxStageTimer`, inflight gauge |
| `observability/reporter` | `src/observability/reporter.rs` | `ReporterConfig::resolve`, heartbeat + sandbox-event wire projection, backoff schedule with 60 s cap, "no heartbeat ever succeeded ⇒ skip unregister" |
| `observability/service` | `src/observability/service.rs` | `node_snapshot` projection, `take_cpu_config_json` take-once semantics |
| `sandbox/network/resolver` | `src/sandbox/network/resolver.rs` | host-netns-pinned DNS worker, bounded queue, 100 ms stop-poll so shutdown/cancel stay responsive, idempotent shutdown |
| `sandbox/network/address_plan` | `src/sandbox/network/address_plan.rs` | added `from_config`; **fixed** default pools (`10.11`/`10.12`/`169.254.0.20/30`) — the previous defaults put the guest `ip=` boot argument on the wrong addresses |
| `snapshot/repository/posixfs/artifacts` | `.../posixfs/artifacts.rs` | managed-layer import (descriptor-trusting vs content-hashed), idempotent content-addressed store with size-mismatch refusal, hard-link-or-verified-copy, `SNAPSHOT_ARTIFACT_LAYOUT` table |
| `storage/overlaybd` LSMT index | `storage/overlaybd/src/lsmt/index.rs` | **fixed** `zeroed` vs `has_physical_range` conflation: backed zeros now keep their physical range through `mend`/`forward_offset_to`/`can_merge_with`, and `ReadOnlyIndex::new` normalizes placeholder offsets |
| `storage/overlaybd` LSMT B+ tree | `storage/overlaybd/src/lsmt/index.rs` | `BptConfig` (u64/u32 fan-outs) + `LinearizedBptree` + `IndexLBPT`; verified differentially against the binary-search `ReadOnlyIndex` over a randomized sweep |
| `sandbox/network/egress_proxy` | `src/sandbox/network/egress_proxy.rs` | preface inspection (HTTP `Host`, TLS SNI reassembled across record boundaries), `normalize_host`, `select_upstream` / `resolve_trusted_upstream` (CIDR grants checked before a hostname is required; resolved addresses re-checked against policy), `SO_ORIGINAL_DST`, and the prepare/activate/discard policy staging state machine |

### Per-module state

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
