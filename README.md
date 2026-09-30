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
| `snapshot/artifact_cache` | `src/snapshot/artifact_cache.rs` | `LocalArtifactCache` (ref-counted LRU cache, concurrent deduplication with per-key mutex+condvar, `EnsureCached` / `PinLocalFile`), `CacheArtifactLease` |
| `snapshot/runtime_support` | `src/snapshot/runtime_support.rs` | `RuntimeImageMaterializer` (derives node-local overlaybd image configs from committed layer refs), `OverlaybdLayerStore` trait, `HydrateRuntimeManifest`, `LoadFirecrackerManifestFromPath` |
| `snapshot/repository/posixfs/runtime` | `src/snapshot/repository/backends/posixfs/runtime.rs` | `PosixFsRuntimeResolver` (resolves committed snapshots to runnable paths: materializes memory/rootfs/drive image configs, resolves attached drives, hydrates runtime manifest) |
| `snapshot/repository/posixfs/backend` | `src/snapshot/repository/backends/posixfs/backend.rs` | `PosixFsBackend` (integrates catalog, artifacts, runtime resolver into a complete repository + resolver bundle) |
| `orchestrator/persistence` | `src/orchestrator/persistence/file_backed.rs` | `FileBackedSandboxPersister` over the local KV store: versioned records, `paused`/`resuming` lifecycle marker so a crash mid-resume is detected and cleaned on load, artifact-root allocation, orphan-artifact sweep. `load_all` takes a `SandboxBackendFactory` and rebuilds each record's `paused_state` through `DecodePausedState`; a record that fails to decode is dropped with its artifacts, and a record from another virtualization mode is loaded visible-but-not-resumable |
| `sandbox/backend` | `src/sandbox/backend.rs` | added `SandboxBackendFactory::DecodePausedState`, which is what makes a persisted paused sandbox actually resumable after a restart |
| `image/cache` deleting GC | `src/image/cache/service.rs` | `ImageCacheGc`: fail-closed collection — a commit is deleted only when no config roots it, no hold pins it, no live runtime references it, and its file is a regular file inside the commit store whose size matches the record. Each candidate is re-checked under GC's own operation hold (ignoring that hold) because source-config publish is not serialised against GC. Unpublish precedes unlink, so a peer is never pointed at bytes that are already gone; a commit with P2P keys and no transport is refused rather than deleted |
| `orchestrator/store` serialisation | `src/orchestrator/store/metadata.rs` | `SandboxMetadata::ToJson` / `FromJson`, with `paused_state` skipped exactly as Rust's `#[serde(skip)]` |
| `p2p/iroh` catalog | `src/p2p/iroh/{catalog,endpoint}.rs` | `PublishedArtifactCatalog` (KV-persisted, so a restart does not silently retract publications peers still hold descriptors for; a corrupt entry fails the load rather than serving a partial catalog), the catalog request/response wire format with its pre-parse size guards, descriptor/provider JSON (externally-tagged, matching serde), and the endpoint backend check. **Not ported**: `CatalogProtocol::accept` and all of `transport.rs` — those are a thin layer over the `iroh` + `iroh-blobs` crates (QUIC endpoint, BLAKE3 verified streaming), which have no C++ equivalent. `iroh.cc` keeps them behind `AGENTENV_WITH_IROH` |
| `api/proxy` routing | `src/api/proxy.rs` | host-route parsing (`<port>-<sandbox-id>.<domain>`, with case/trailing-dot/port normalisation; a *malformed* sandbox host is an error rather than a fallthrough to the API), routing-header parsing with its E2B aliases, `single_header` (a repeated credential counts as absent), `strip_host_port` (leaves a bracketless IPv6 literal intact), and the full `ProxyRequestError` → status map. **Not ported**: the forwarding half, which needs an HTTP client |
| `api/impls/auth` | `src/api/impls/auth.rs` | the authorization decision as a pure function: API key on the control plane (with template-builder sandboxes hidden behind 404 so the public API does not reveal the id), and the sandbox's own policy on the data plane — envd-port requests need the envd token when the sandbox is `secure`, everything else needs public traffic or a traffic token. Credential stripping is preserved as part of the contract: an API key that authenticated a proxy request, and any envd token that did *not* authorize it, are removed before the request reaches the guest |
| `api/impls/volumes` | `src/api/impls/volumes.rs` | `ResolveVolumeMounts` with its reservation rollback — a failure anywhere in the loop releases every lease the call made, so a half-resolved request cannot leave volumes leased to a sandbox that never launched. Mount-path overlap is checked **component-wise** (Rust's `Path::starts_with`), so `/mnt/a` and `/mnt/ab` may coexist while `/mnt/a` and `/mnt/a/b` are rejected. Plus the `VolumeError` → status map (the 409 group being "exists but unusable right now"), the record projection (internal fields withheld), and mode parsing that rejects rather than defaults. Handler methods themselves are bound to the generated route enums and are not ported |
| `api/impls/admin` | `src/api/impls/admin.rs` | `NodeSnapshot` → `Node` / `NodeDetail` projection, keeping the one shape difference (`sandboxStartingCount` is collection-only) and reporting paused reservations separately from live allocation. Disabled observability yields an empty list rather than a synthetic node, because a partial node is indistinguishable from a real one to a scheduler |
| `api/impls/attached_drives` | `src/api/impls/attached_drives.rs` | validation order preserved (drive id → mount path → sub path → size → image), uniqueness enforced on the *resolved* mount path so two drives that both omit it collide here rather than at launch, `diskSizeMB` required to be ≥1024 and GiB-aligned, and image resolution deferred to a second pass so a malformed request fails before any registry work |
| `snapshot/p2p` | `src/snapshot/p2p.rs` | `SnapshotP2pArtifact`: fixed per-snapshot keys (`snapshot/v1/artifacts/<id>/<name>`) plus digest- and uuid-keyed overlaybd layer artifacts. `local_overlaybd_layers` reproduces both publication guards — a raw layer whose digest is absent from the committed record is *not* published (publish-time compression recontainerized it as zfile, so the raw-digest key would never be looked up), and uuid-keyed publication is gated on the committed uuid set. Every failure path is a skip-with-warning, since P2P is an optional acceleration path |
| `storage/overlaybd` layer metadata | `storage/overlaybd/src/layer/layer_metadata.rs` | `ReadOverlaybdLayerUuid` / `ReadOverlaybdLayerVirtualSize` over the sealed trailer, with Rust's "unparseable uuid ⇒ nil" behaviour preserved (nil is the empty string here) |
| `image/cache` metadata graph | `src/image/cache/graph.rs` | `ImageCacheMetadataStore`: schema-version reconcile (wipes on mismatch), hard-commit object records with merge-preserving P2P key sets, config→hard and the mirrored hold→hard / hard→hold edges, `rebuild_from_configs` that fails closed on a malformed config, LRU touch on every resolve, and the store-backed capacity-eviction planner |

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
