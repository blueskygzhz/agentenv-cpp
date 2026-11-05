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

C++11: kept modular but simpler. The names below are the real ones — an
earlier revision of this section listed `LocalRepository` / `S3Repository` /
`MockRepository`, none of which exist:

- `snapshot::repository::SnapshotRepository` is the interface, split into a
  **catalog store** (documents: snapshot records, volume records, aliases) and
  an **artifact store** (content-addressed blobs / overlaybd layers).
- Implementations: `PosixFsSnapshotRepository` (`PosixFsCatalogStore` +
  `PosixFsArtifactStore`, rooted at
  `{backend.posix_fs.snapshot_store}/repository`) and
  `InMemorySnapshotRepository` (tests).
- **The posix-fs record layer now comes from the shared catalog.** Upstream's
  `backends/posixfs/catalog.rs` has *only* document methods; records come from
  the shared document-backed catalog (ported as `record_catalog.h`, see
  section 7). `PosixFsCatalogStore` used to carry a second record
  implementation of its own — record paths, alias bindings, `flock` — over the
  very same files; those methods are gone and
  `PosixFsSnapshotRepository` forwards to `record_catalog` as the OSS backend
  does. No migration was needed: `DocumentKey::SnapshotRecord(id).PosixPath()`
  resolves to `catalog/records/<id>.json` and its lock to
  `catalog/records/<id>.lock`, which is byte for byte what the removed code
  used, and an alias is still `catalog/aliases/<name>` holding the owner's
  uuid. (An earlier revision of this document claimed the two layouts did not
  share a key space and that records written here were invisible to a Rust
  node. That was wrong — the paths were always identical.) What the move did
  fix is four behavioural divergences the local copy had: the `deleting`
  marker did not gate reads, a failed alias claim left the record behind,
  records were written compact rather than pretty, and `delete` only unlinked
  files instead of running upstream's full sequence.
- `main.cc` constructs the posix-fs pair directly. `MakePosixFsRepository`
  still returns the in-memory stub (it has no `OverlaybdLayerStore` or
  `LocalArtifactCache` to build the real one with), while
  `MakeOssRepository(const cfg::OssBackendConfig&)` now builds a real
  `OssSnapshotRepository`. So **`[backend.oss]` is reachable for documents,
  blobs and snapshot records** — see the OSS section below.
- The OSS document layer (`repository/oss/documents.{h,cc}`) is ported: shared
  catalog documents are serialised across nodes with an **ETag
  compare-and-swap** (read with a version token, write `If-Match`, create with
  `If-None-Match`, retry with jittered backoff up to 32 attempts). It talks to
  an `ObjectStore` interface rather than to HTTP, which is what makes the race
  behaviour testable without a server — see `tests/test_oss_documents.cc`,
  whose fake injects a competing writer between the read and the write.
  - Three read states stay distinct and must not be merged: `Found`,
    `NotFound`, and `Changed` (the object was replaced between its stat and
    its read). Folding `Changed` into `NotFound` makes the update write
    create-only, which can never succeed while the object exists — a hot
    document would stop being updatable.
  - An **append-only** store (Alibaba's native OSS) cannot write
    conditionally, so such a store is *refused* rather than run through the
    CAS: a precondition the store ignores is no guarantee at all, and the lost
    update would leave no symptom. Upstream serialises writes there with a
    document lease, and **that lease is now ported**
    (`repository/oss/lease.{h,cc}`, section 7a); what is still missing is the
    one store operation it stands on — see that section — so the refusal
    stays in the document layer rather than surfacing from inside an acquire.
- `OssClient` (`repository/oss/client.{h,cc}`) is the S3/MinIO `ObjectStore`
  over HTTP: SigV4-signed GET / PUT / DELETE / ListObjectsV2 on the existing
  `http::Transport` and `S3SigningContext`, with the transport injected so
  `tests/test_oss_client.cc` exercises it without a server. It is *not* the
  `storage/overlaybd/oss.h` `OssBackend`, which is a read path tuned for
  ranged layer reads and has no conditional write, list, or delete.
  - `OssClientConfig::FromBackendConfig` rejects a `[backend.oss]` section that
    is missing bucket / endpoint / region / credentials (blank counts as
    missing), and reports `credential_process` as unsupported rather than
    signing with nothing.
  - `CanAppend` is true exactly for Alibaba's own hosts (`.aliyuncs.com`,
    `.aliyun-inc.com`), matched on the host suffix so
    `aliyuncs.com.evil.example` does not qualify. Such a client is therefore
    refused by the document layer (above) before it issues a request.
  - `GetWithEtag` is a single GET, so this client never answers `Changed`:
    bytes and ETag come from one response and cannot disagree. The state stays
    in the interface because upstream's stat-then-read backend can produce it.
  - 412 and 409 are a *lost race* (`false`), not an error; 404 on DELETE is
    success; 5xx and transport failures are retried, 4xx is returned as-is;
    a 403 is re-signed once in case the clock skewed, then surfaces.
  - The ListObjectsV2 query is built in sorted order because SigV4 hashes the
    canonical query string; a page is capped at the server's 1000, and a key
    outside the configured prefix is rejected rather than relativised into a
    document that does not exist.
  - Only the *paged* listing is ported. Upstream also has `read_all` /
    `list_keys_recursive`, which walk a whole collection with 16 reads in
    flight and skip a document deleted since the listing; nothing here needs
    them yet, and a serial stand-in would quietly change the cost of whatever
    first does.
- `OssSnapshotRepository` (`repository/oss/repository.{h,cc}`) assembles the
  two halves behind `SnapshotRepository`, and `MakeOssRepository` returns it.
  Its record methods forward to the shared catalog below; only `Delete` is
  still refused.
  - The document half being complete is what makes the volume catalog
    (`volume/catalog.h`, built on documents exactly as upstream) usable over
    object storage today.
  - Blobs: `HasBlob` is a `HEAD`, so probing a multi-gigabyte layer costs one
    round trip — and a 403 there is an error rather than "absent", or a
    publish would skip an upload it never verified. `LocateBlob` is always
    `Remote`, since no node can reach the bytes by path. A layer is skipped
    when it already exists (its digest names its contents), while a snapshot
    file is replaced unconditionally.
  - `FetchBlob` streams to the destination through a sink that is restarted
    before each attempt and only creates the file once bytes arrive, so a
    failed fetch leaves nothing behind and a retry cannot splice two responses
    into a file of plausible length. A body shorter than its own
    `Content-Length` is refused with the partial file removed.
  - A file-bodied `PutBlob` goes to an injected `ObjectUploader`, whose
    production implementation is `OssBackend::UploadPath` — the multipart
    upload this tree already had (and whose first caller this is). The seam
    exists because that code signs through the global libcurl transport and
    cannot be driven by the client's injected one: routing stays testable
    without a server, and the protocol stays one implementation.
- `record_catalog.{h,cc}` is the record layer **every** backend shares (Rust
  `impl<R: Repository + ?Sized> SnapshotCatalog for R`, `src/snapshot/catalog.rs`).
  Upstream's `Repository` trait has no record methods: a snapshot record is an
  ordinary *document*, and `create` / `get` / `list` / `resolve_alias` /
  `try_start_build` / `mark_build_error` come from one blanket impl over the
  document methods. A free-function set here, since C++11 has no blanket impl;
  a backend forwards its record methods to it, and the forwarding cannot
  recurse because these functions call only the document methods.
  - Why it matters that this is shared and not per-backend: every backend then
    keeps records under the *same* key space, which is the only reason a node
    on one backend can read a catalog written by a node on another.
  - `UpdateRecord` runs its decision function **twice** — once inside the
    atomic update to choose what to write, once on the state the write was
    actually applied to, to produce the answer. That is what makes the
    reported outcome describe the state the write landed on rather than the
    state first read, which on a contended record are different; it is also
    why the function must be pure.
  - `deleting` gates everything: `get`, `resolve_alias`, `list`,
    `try_start_build` and `mark_build_error` all treat a marked record as
    absent, because its artifacts may already be gone and a caller that
    resumed from them would fail as if they were corrupt. The record itself
    stays readable by id so a retried delete can finish.
  - `create` claims the alias *after* writing the record and rolls the record
    back if the claim is lost, since a record holding an alias it does not own
    is findable by id, invisible by name, and reconciled by nothing.
  - `claim_alias` yields to a *live* owner only. An alias whose snapshot is
    gone is cleaned up on sight (`alias_owner`), or a name left behind by a
    half-finished delete would be blocked forever.
  - `list` fails on a record that will not parse rather than skipping it: a
    quietly short catalog is indistinguishable from a smaller one.
  - `delete` runs upstream's full sequence: mark `deleting` → release the
    alias → remove the artifacts (`Repository::delete_snapshot_blobs`) →
    delete the build log → delete the record. The order *is* the recovery
    story: every step is idempotent, the record is the only thing that names
    those files, so it must outlive them, and a marked record is already
    invisible to readers while it waits for the retry. Reversing the last two
    would leave an unreferenced directory nothing could find again.
    - Deleting by an id whose record is already gone still removes the build
      log, which is how an attempt that died right after the record was
      removed gets finished.
    - A snapshot with a **disk-image publication** is refused, from *inside*
      the atomic update so the record is not even marked: rolling a
      publication back needs Rust's `AcrDiskImageExporter`, which is not
      ported, and marking-then-stopping would hide the snapshot from every
      read while its image stayed live in the registry.
  - `delete_snapshot_blobs` is the backend's own half. `posixfs` removes the
    snapshot directory and `fsync`s its parent (without that, a crash could
    leave the record durably deleted and the files resurrected as garbage);
    `oss` lists and deletes the snapshot's key prefix **with a trailing
    slash**, without which the prefix of one id also matches any id that
    starts with it. A missing file or object is success, because this runs
    before the record is deleted and has to be retryable. Content-addressed
    **managed layers stay** in both: they live outside the snapshot and may
    belong to other snapshots.
  - Also not ported from that file: `publish` / `publish_with_local_layers` /
    `commit` / `attach_startup_manifest`, which need `publish.rs` and
    `store.rs`.
  - Both backends answer their record methods from here. `posixfs` used to
    keep a second implementation over the same files; it is gone — see
    section 5.

### 7a. The OSS document lease

- `repository/oss/lease.{h,cc}` (Rust `backends/oss/lease.rs`) is what
  upstream uses where there is no conditional write: Alibaba's native OSS.
  A document's lock is an **appendable object of JSON lines** under `locks/`,
  a prefix older nodes never list. OSS appends only at an object's current
  length, so of two appends at one length exactly one lands.
  - Each line is a frame naming the offset it must land at and the holder it
    replaces. A frame counts only if *both* match, which is what makes a claim
    appended from an outdated view inert — including the case where the lock
    was deleted and grew back to the same length under someone else. The last
    frame that counts holds the lock.
  - A line that does not parse is **skipped but still counted** towards the
    offsets: another version may append lines this build cannot read, and both
    views must still agree on where the next append lands.
  - An append that reports no offset may still have landed, so a claim is
    confirmed by **reading the object back**, never by the reply alone — the
    one exception being an append that landed at offset 0 of an object that
    did not exist, which cannot be anyone else's.
  - A holder that died never releases, so a contender that has seen the *same*
    holder for 30 s takes the lock over. A merely slow holder cannot be
    stopped, so holders bound themselves: `Lease::deadline_ms` is 10 s before
    any contender may take over, dated from when the claiming append was
    **sent** (not confirmed), and every write made under the lease must start
    before it. `static_assert` keeps that margin larger than two write
    attempts, because a write may take a second one after refreshing
    credentials.
  - `Release` deletes the lock object, and reports nothing: a release that
    could not be made durable is logged and the lock left to expire, because
    failing here would make a write that *did* land look like it had not.
  - Upstream bounds an acquire with `tokio::time::timeout`, which cancels the
    request in flight. Nothing here can cancel a blocking call, so the budget
    is checked between attempts and the bound becomes 75 s plus the one
    request already in flight.
  - **What is still missing**: `LockStore::AppendLock`. Appending is Alibaba's
    own `AppendObject`, signed with `OSS4-HMAC-SHA256` against an
    `oss-<region>.aliyuncs.com` endpoint — a different service from the S3
    REST API `OssClient` speaks, which is why upstream builds a second
    opendal operator (`native_oss`) for it. Until that transport exists the
    document layer keeps refusing an append-only store.
  - Tests (`tests/test_oss_lease.cc`) split the two halves the way the port
    does: the timing rules run on an injected virtual clock (a 30 s lease
    cannot be waited out for real), and mutual exclusion runs on 16 real
    threads, because two appends actually racing is the whole point.
- Cross-node sharing therefore means *one shared POSIX filesystem*. Durable
  state (catalog documents, managed layers) lives in the repository and is
  serialised with `flock` + `mkstemp`/`rename` + read-modify-write CAS;
  node-local state (`image.json`, ublk devices) is rebuilt per node by
  `MaterializeVolumeBacking` and never persisted.
- `ArtifactStore::FetchBlob` copies a blob out of the repository into a
  caller-owned destination, which is what lets a node that does *not* share
  the filesystem still obtain a layer. `PosixFsArtifactStore` implements it as
  a size-verified `fs::copy`, as upstream does — the destination is
  caller-owned, so staging it atomically is the caller's concern. A repository
  with no artifact store (`InMemorySnapshotRepository` included) keeps the base
  class's `unsupported("blob storage")` answer.
- P2P layer (`p2p::Node`) is a **stubbed interface**; you can plug a real
  transport later. `src/p2p/iroh.cc` is a compile-time `#error` unless
  `AGENTENV_WITH_IROH`, and every operation reports
  `P2pError::MakeDisabled()`.

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
