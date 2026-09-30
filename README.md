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
| `template/runner` (in-sandbox half) | `src/template/runner.rs` | Default-user provisioning and the ready-check loop. envd resolves every filesystem operation against the template's default user and the e2b SDK assumes that account exists (`from_dockerfile` injects `USER user`), so an arbitrary OCI image without it would fail every later SDK call — the account is created at build time instead. The provisioning script *dispatches* on which tool the image ships rather than chaining `useradd \|\| adduser`, because a chain would mask a real failure of the present tool behind the fallback's "command not found"; exit 66 is reserved for "no tooling at all", the one failure that stays a warning. The ready loop re-checks the start command every iteration so a dead start command reports *its own* failure instead of a ready-command timeout, and treats a clean exit as "done" rather than "dead". Orchestration (`build_template`, `run_template_build`) needs the Firecracker backend |
| `template/builder` (preparation half) | `src/template/builder.rs` | Validates a spec into a `TemplateBuildContext`. The snapshot-based path carries the weight: building on a committed snapshot *resumes* it, so the CPU/memory shape is baked into the capture and cannot be changed, and a capture taken under a different virtualization mode cannot be resumed at all — both are refused up front with a precise reason rather than failing deep in the build. A startup override that names no shell inherits the base's (the base image may only be usable under it), but the alias is deliberately *not* inherited, or a derived build could claim the base's name. `execute_and_publish` needs the runner and is not yet ported |
| `template/step_executor` | `src/template/step_executor.rs` | Folds build steps over a `CommandContext`. WORKDIR **creates** the directory (Docker does, and both Dockerfile front-ends map onto this step) via the executor's filesystem service rather than exec'ing `mkdir`, because scratch/distroless images ship no userland. A failure's `reason.step` is the *client-visible* number — `source_step` when a front-end expanded one request step into several, the 1-based position otherwise — because the e2b SDKs parse that field as an integer index into their own stack traces and a wrong value points at the wrong entry. `ResolveWorkdir` normalizes lexically (the directory may not exist yet) and cannot escape the root |
| `sandbox/firecracker` process-VM + lifecycle | `src/sandbox/firecracker/{process_vm_reader,pool,startup_pack,config}.rs` | `ProcessVmReader` pulls guest memory out of the live VMM with `process_vm_readv`, which is what makes a direct memory snapshot possible. Two contracts are load-bearing: the offset is a **host virtual address**, not a file offset; and a **short read must be resumed**, because a partial read left as zeros is indistinguishable from genuinely zeroed guest memory and would corrupt the snapshot silently (a zero-byte return is not progress and must error, or the loop never terminates). Also fixes a real bug in `ValidateExtraDriveSet`: it compared mount paths for *equality*, while Rust checks for **overlap** after normalisation — so `/mnt/a` plus `/mnt/a/b` was accepted, leaving the nested drive shadowed and unreachable in the guest. The overlap rule now lives in `sandbox/extra_drive.h` and is shared with volume mounts, since two copies could drift |
| `core/http` | `hyper` + `hyper-util` legacy `Client` | Hand-written HTTP/1.1 client over AF_UNIX and TCP; no third-party dependency. Three things a real client must get right and the previous ad-hoc socket code did not: **the status code is part of the result** (Firecracker answers a bad request with 400 plus a JSON fault body, which the old code returned as if it were a 204 success, so the caller carried on and failed much later somewhere unrelated); **both response framings** are decoded, so a chunked reply never reaches the caller with chunk headers still embedded; and **connect/read are bounded**, because with a thread-pool executor a wedged peer would otherwise cost a worker permanently. Writes use `send(MSG_NOSIGNAL)` — a peer dying mid-request would otherwise raise SIGPIPE and kill the whole process. Async is `core::Executor`: the blocking call moves to a pool thread, which is the same shape as awaiting on Tokio for I/O-bound work. `Connect` is lazy, matching Rust's infallible `UnixSocketClient::new` |
| `sandbox/firecracker` dirty-page mapping | `src/sandbox/firecracker/overlaybd_snapshot.rs` | Converts Firecracker dirty memory ranges into overlaybd segment mappings. This straddles two alignments — Firecracker reports 4096-byte pages, overlaybd addresses 512-byte sectors — and every mapping field (`offset`/`length`/`moffset`) is a *sector* count. An off-by-one in that division silently writes the wrong guest pages, surfacing much later as a VM resuming into corrupt memory, so every input is validated instead of trusted: page size must be exactly 4096, ranges must be page-aligned and non-empty, and a range running past the declared memory size is refused. Long runs are split at `Segment::kMaxLength` while staying contiguous on both sides. The final overlap check is on **destination** sectors only — two ranges may legitimately share a host virtual address, but two mappings on one image sector would make the snapshot depend on layer ordering. `SplitRuntimeSuffix` partitions a layer stack at the first runtime-owned layer and takes *everything* above it, since an ordered stack means a durable layer above a runtime layer still depends on it |
| `api/impls/sandbox` (validation half) | `src/api/impls/sandbox.rs` | Request validation and projection. The load-bearing rule is the domain allowlist: domain inspection only covers HTTP/HTTPS, so a policy naming domains *without* an explicit ALL_TRAFFIC deny would leave every other port open while reading as a restriction — it is refused. IPv6 CIDRs are rejected rather than ignored (the runtime installs only v4 rules, so an accepted v6 rule would be an unenforced restriction), `diskSizeMB` must be a whole number of GiB because the block layer allocates in 1024 MiB units, and `allowInternetAccess` is a genuine tri-state that maps `Default` back to JSON null so a client can tell "not set" from "denied". Handlers stay with the generated route enums |
| `api/impls/image_build` (decisions) | `src/api/impls/image_build.rs` | Dockerfile HEALTHCHECK translation and the build-session state machine. A mis-rendered healthcheck becomes the template's readiness gate, so an unrecognised form is an error rather than a silent skip — "always ready" would hand out a sandbox before it works. Exec-form arguments are quoted individually because the pieces are re-joined into one shell string. Every session transition is one-way and guarded: a cancelled build can never be observed as ready (which is what stops a client tunnelling into a sandbox being torn down), publishing requires having been ready, and terminal states never re-transition. The tunnel/worker half needs the transport stack |
| `image/cache` operation holds | `src/image/cache/service.rs` | The lease that stops GC collecting commits an in-flight resolve/import/GC depends on. Owners are uniquely numbered even for the same operation name: sharing one would let the first operation to finish release the second's protection while it still runs. The lazy form writes nothing until the first commit is actually protected, because most resolves hit a cached config and protect nothing. Release is best-effort and never escalates — leaving a hold behind costs disk, dropping one early costs a running operation its inputs. Rust's `Drop` becomes RAII |
| `api/impls/template_helpers` | `src/api/impls/template_helpers.rs` | Translates an E2B-shaped build request into a `TemplateBuildSpec`. The load-bearing detail is step numbering: one client step can expand into several internal steps (`ENV A=1 B=2` → two `Env` steps), and `StampSourceSteps` back-fills the client index over that range so both halves report as client step 1 rather than shifting every later step's reported position. Also: unsupported inputs are rejected rather than ignored (a non-blank `filesHash` means the client staged files for a COPY; `name:tag` would silently build the untagged template), and error messages quote the caller's original step spelling, not the upper-cased match key. The request DTOs are generated types upstream and are declared as plain data here |
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
