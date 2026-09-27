# Rust → C++11 Idiom Mapping

This is the **translation table** the port follows. It is deliberately conservative:
we only use C++11 language features and the C++11 standard library (plus `pthread`
and Linux headers where needed). We list the C++17/20 upgrade path in the last column
for reference.

## 1. Ownership & lifetime

| Rust                    | C++11 equivalent                              | Notes                                    | C++17+ upgrade                      |
|-------------------------|-----------------------------------------------|------------------------------------------|-------------------------------------|
| `Box<T>`                | `std::unique_ptr<T>`                          | unique ownership, heap                   | same                                |
| `Arc<T>`                | `std::shared_ptr<T>`                          | atomic refcount                          | same                                |
| `Rc<T>`                 | `std::shared_ptr<T>` (non-atomic variant N/A) | C++11 has no non-atomic sp               | manual `intrusive_ptr`              |
| `Weak<T>`               | `std::weak_ptr<T>`                            |                                          | same                                |
| `&T` / `&mut T`         | `const T&` / `T&`                             | ban dangling by convention               | `std::span<T>` for slices           |
| `T` (moved)             | `T&&` + `std::move`                           | move semantics available in C++11        | same                                |
| `Cow<'a, T>`            | user-defined `Cow<T>` (holds shared/owned)    | ~50 loc                                  | same                                |
| `Pin<Box<T>>`           | `std::unique_ptr<T>`                          | rare, only for self-refs                 | same                                |

## 2. Error handling

| Rust                    | C++11 equivalent                              | Notes                                    |
|-------------------------|-----------------------------------------------|------------------------------------------|
| `Result<T, E>`          | `agentenv::core::Expected<T, E>`              | see `include/agentenv/core/expected.h`. |
| `Option<T>`             | `agentenv::core::Optional<T>`                 | tagged union; C++17 has `std::optional`  |
| `?` operator            | `AGENTENV_TRY(expr)` macro                    | expands to `if (!e.ok()) return e.err()` |
| `anyhow::Error`         | `agentenv::core::AnyError`                    | erased error with source chain           |
| `thiserror`             | enum classes + `error_category`               | one enum per module                       |
| `panic!`                | `std::abort()` after logging                  | reserved for invariant breaks            |

## 3. Traits ↔ Abstract classes

| Rust idiom                              | C++11 idiom                                              |
|-----------------------------------------|----------------------------------------------------------|
| `trait Foo { fn bar(&self); }`          | `class Foo { public: virtual void bar() const = 0; virtual ~Foo() = default; };` |
| `impl Foo for X { ... }`                | `class X final : public Foo { void bar() const override { ... } };`              |
| `dyn Foo`                               | `Foo&` / `std::shared_ptr<Foo>`                          |
| `impl Foo` (return type)                | template return, or `std::unique_ptr<Foo>`               |
| supertrait `trait Foo: Send + Sync`     | comment; enforce via review — C++ has no `Send/Sync`     |
| associated type `type Item;`            | `using Item = ...;`                                      |
| trait object `Box<dyn Foo + Send>`      | `std::unique_ptr<Foo>` (send-safe by contract)           |
| `async fn` in trait                     | `virtual std::future<T> bar() = 0;` (see §5)             |

## 4. Concurrency

| Rust                                | C++11 equivalent                                                    |
|-------------------------------------|---------------------------------------------------------------------|
| `std::thread::spawn(f)`             | `std::thread(f).detach()` or thread pool                            |
| `tokio::spawn(async fn)`            | `pool.submit([](){...})` returning `std::future<T>`                 |
| `tokio::sync::Mutex<T>`             | `std::mutex` + guard                                                |
| `std::sync::Mutex<T>`               | `std::mutex`                                                        |
| `parking_lot::RwLock<T>`            | `std::shared_timed_mutex` (C++14) or user impl                      |
| `AtomicU64`                         | `std::atomic<uint64_t>`                                             |
| `crossbeam::channel::unbounded()`   | `agentenv::core::MpscQueue<T>`                                      |
| `tokio::sync::oneshot`              | `std::promise<T>` + `.get_future()`                                 |
| `select! { ... }`                   | future-set with cv or `epoll`; no real equivalent                   |
| `join!(a, b)`                       | `a.get(); b.get();` or `wait_all(vec<future>)`                      |

## 5. Async ↔ synchronous port strategy

Rust's `tokio` runtime is deeply integrated with the whole codebase. In C++11:

1. **APIs return `std::future<Expected<T, E>>`** — semantically `async fn -> Result<T, E>`.
2. **Implementation runs on a `core::Executor`** (thread pool, N = hw threads).
3. **HTTP handlers** submit work to the executor.
4. **io_uring** work runs on one dedicated ring per NUMA node.
5. **Cancellation**: `core::CancelToken` cooperatively checked at await points.

## 6. Serde ↔ JSON serialization

- Every DTO gets `to_json()` + `static from_json(const std::string&)`.
- Hand-rolled `agentenv::core::json`. Drop-in replaceable with `nlohmann/json`.

## 7. Channels

`crossbeam::channel::{unbounded, bounded}` → `core::MpscQueue<T>`.

## 8. Modules ↔ Namespaces

Rust `crate::foo::bar` → C++ `agentenv::foo::bar`.

## 9. Cargo features ↔ CMake options

Rust `#[cfg(feature = "grpc")]` → CMake `AGENTENV_WITH_GRPC` → `#ifdef AGENTENV_WITH_GRPC`.

## 10. Testing

`#[test]` fn's → `microtest::MT_TEST(...)` (drop-in GoogleTest later). No property-based tests ported.

---

# 11. File-level 1:1 correspondence (Rust source → C++ header / cc)

The C++ port keeps a strict 1:1 mapping between Rust source files and C++ headers.
Each C++ header carries a `// Rust: <path>` comment as documentation. Multi-file Rust
sub-modules are aggregated into `_all.h` / `_all.cc` pairs where breaking them apart
would only produce trivial empty translation units.

| Rust source                                          | C++ header                                                        | C++ source                                        |
|------------------------------------------------------|-------------------------------------------------------------------|---------------------------------------------------|
| `src/lib.rs` (crate root)                            | —                                                                 | —                                                 |
| `src/cfg.rs`                                         | `include/agentenv/cfg.h`                                          | `src/cfg.cc`                                      |
| `src/digest.rs`                                      | `include/agentenv/core/digest.h`                                  | `src/core/digest.cc`                              |
| `src/identity.rs`                                    | `include/agentenv/core/identity.h`                                | `src/core/identity.cc`                            |
| `src/local_store.rs`                                 | `include/agentenv/local_store.h`                                  | `src/local_store.cc`                              |
| `src/logging.rs`                                     | `include/agentenv/core/logging.h`                                 | `src/core/logging.cc`                             |
| `src/privileges.rs`                                  | `include/agentenv/privileges.h`                                   | `src/privileges.cc`                               |
| `src/proto.rs`                                       | `include/agentenv/proto.h`                                        | `src/proto.cc`                                    |
| `src/orchestrator/mod.rs`                            | (namespace `agentenv::orchestrator`)                              | —                                                 |
| `src/orchestrator/types.rs`                          | `include/agentenv/orchestrator/types.h`                           | `src/orchestrator/types.cc`                       |
| `src/orchestrator/store.rs`                          | `include/agentenv/orchestrator/store.h`                           | `src/orchestrator/store.cc`                       |
| `src/orchestrator/metrics.rs`                        | `include/agentenv/orchestrator/metrics.h`                         | `src/orchestrator/metrics.cc`                     |
| `src/orchestrator/launch_plan.rs`                    | `include/agentenv/orchestrator/launch_plan.h`                     | `src/orchestrator/launch_plan.cc`                 |
| `src/orchestrator/proxy.rs`                          | `include/agentenv/orchestrator/proxy.h`                           | `src/orchestrator/proxy.cc`                       |
| `src/orchestrator/service.rs`                        | `include/agentenv/orchestrator/service.h`                         | `src/orchestrator/service.cc`                     |
| `src/orchestrator/persistence/*.rs`                  | `include/agentenv/orchestrator/persistence.h`                     | `src/orchestrator/persistence.cc`                 |
| `src/sandbox/mod.rs`                                 | `include/agentenv/sandbox/types.h`                                | —                                                 |
| `src/sandbox/backend.rs`                             | `include/agentenv/sandbox/backend.h`                              | `src/sandbox/backend.cc`                          |
| `src/sandbox/mock.rs`                                | `include/agentenv/sandbox/mock.h`                                 | `src/sandbox/mock.cc`                             |
| `src/sandbox/envd.rs`                                | `include/agentenv/sandbox/envd.h`                                 | `src/sandbox/envd.cc`                             |
| `src/sandbox/extra_drive.rs`                         | `include/agentenv/sandbox/extra_drive.h`                          | `src/sandbox/extra_drive.cc`                      |
| `src/sandbox/process.rs`                             | `include/agentenv/sandbox/process.h`                              | `src/sandbox/process.cc`                          |
| `src/sandbox/network/*.rs` (5 files)                 | `include/agentenv/sandbox/network.h`                              | `src/sandbox/network.cc`                          |
| `src/sandbox/custom_extension/*.rs` (2 files)        | `include/agentenv/sandbox/custom_extension.h`                     | `src/sandbox/custom_extension.cc`                 |
| `src/sandbox/ublk/*.rs` (3 files)                    | `include/agentenv/sandbox/ublk.h`                                 | `src/sandbox/ublk.cc`                             |
| `src/sandbox/firecracker/*.rs` (12 files)| `include/agentenv/sandbox/firecracker/*.h`       | `src/sandbox/firecracker/*.cc` (config/socket/mmds/instance/pool/sandbox) |
| `src/snapshot/mod.rs`                                | (namespace `agentenv::snapshot`)                                  | —                                                 |
| `src/snapshot/types/*.rs`                            | `include/agentenv/snapshot/types.h`                               | —                                                 |
| `src/snapshot/manager.rs`                            | `include/agentenv/snapshot/manager.h`                             | `src/snapshot/manager.cc`                         |
| `src/snapshot/mock.rs`                               | `include/agentenv/snapshot/mock.h`                                | `src/snapshot/mock.cc`                            |
| `src/snapshot/artifact_cache.rs`                     | `include/agentenv/snapshot/artifact_cache.h`                      | `src/snapshot/artifact_cache.cc`                  |
| `src/snapshot/p2p.rs`                                | `include/agentenv/snapshot/p2p.h`                                 | `src/snapshot/p2p.cc`                             |
| `src/snapshot/runtime_support.rs`                    | `include/agentenv/snapshot/runtime_support.h`                     | `src/snapshot/runtime_support.cc`                 |
| `src/snapshot/repository/*.rs` (22 files)            | `include/agentenv/snapshot/repository.h`                          | `src/snapshot/repository.cc`                      |
| `src/image/mod.rs`                                   | `include/agentenv/image/image.h`                                  | `src/image/image.cc`                              |
| `src/image/reference.rs`                             | `include/agentenv/image/reference.h`                              | `src/image/reference.cc`                          |
| `src/image/metadata.rs`                              | `include/agentenv/image/metadata.h`                               | `src/image/metadata.cc`                           |
| `src/image/{local_layer,resolver,cache}.rs` + oci_image/commit_index (own rows below) | `include/agentenv/image/layers.h`                    | `src/image/layers.cc`                             |
| `src/p2p/mod.rs`                                     | `include/agentenv/p2p/node.h`                                     | `src/p2p/node.cc`                                 |
| `src/p2p/{config,transport,discovery,types,error,iroh,mock}.rs` | `include/agentenv/p2p/all.h`                          | `src/p2p/all.cc`                                  |
| `src/api/mod.rs`                                     | (namespace `agentenv::api`)                                       | —                                                 |
| `src/api/server.rs`                                  | `include/agentenv/api/server.h`                                   | `src/api/server.cc`                               |
| `src/api/proxy.rs`                                   | `include/agentenv/api/proxy.h`                                    | `src/api/proxy.cc`                                |
| `src/api/impls/*.rs` (9 files)                       | `include/agentenv/api/impls.h`                                    | `src/api/impls.cc`                                |
| `src/api/dto/*.rs`                                   | `include/agentenv/api/dto.h`                                      | `src/api/dto.cc`                                  |
| `src/setup/*.rs` (7 files)                           | `include/agentenv/setup.h`                                        | `src/setup.cc`                                    |
| `src/template/mod.rs`                                | `include/agentenv/template/engine.h`                              | `src/template/engine.cc`                          |
| `src/observability/mod.rs` + `crates/observability/` | `include/agentenv/observability/{metrics,tracing}.h`              | `crates/observability/{metrics,tracing}.cc`       |
| `storage/util/`                                      | `include/agentenv/storage/util/types.h`                           | `storage/util/types.cc`                           |
| `storage/overlaybd/`                                 | `include/agentenv/storage/overlaybd/types.h`                      | `storage/overlaybd/{index_tree,layer_reader}.cc`  |
| `storage/ublk/`                                      | `include/agentenv/storage/ublk/target.h`                          | `storage/ublk/runner.cc`                          |
| `storage/ublk-daemon/`                               | (bin)                                                             | `storage/ublk-daemon/main.cc`                     |
| `storage/uffd-core/`                                 | `include/agentenv/storage/uffd-core/uffd.h`                       | `storage/uffd-core/uffd.cc`                       |
| `crates/aenv/src/main.rs`                            | `include/agentenv/aenv/cli.h`                                     | `crates/aenv/{main,cli}.cc`                       |
| `crates/aenv/src/commands/*.rs` (15 files)      | `include/agentenv/aenv/commands.h`         | `crates/aenv/commands/*.cc` (15 files + mod.cc)   |
| `crates/aenv/src/client/*.rs` (5 files)              | `include/agentenv/aenv/client.h`                                  | `crates/aenv/client.cc`                           |
| `crates/aenv/src/{auth,grpc,output,progress,pty}.rs` | `include/agentenv/aenv/util.h`                                    | `crates/aenv/util.cc`                             |
| `crates/linux-cap/`                                  | `include/agentenv/linux-cap/cap.h`                                | `crates/linux-cap/cap.cc`                         |
| `crates/shell-util/`                                 | `include/agentenv/shell-util/shell.h`                             | `crates/shell-util/shell.cc`                      |
| `crates/warm-pool/`                                  | `include/agentenv/warm-pool/pool.h` (header-only `WarmPool<T>`)    | — (no .cc; INTERFACE target)                      |
| `crates/object-store-operator/`                      | `include/agentenv/object-store-operator/operator.h`               | `crates/object-store-operator/operator.cc`        |
| `services/scheduler/*.go` (Go)                       | (bin)                                                             | `services/scheduler/main.cc`                      |
| `services/gateway/*.go` (Go)                         | (bin)                                                             | `services/gateway/main.cc`                        |
| `services/api/*.go` (Go)                             | (bin)                                                             | `services/api/main.cc`                            |
| `services/config/*.go` (Go)                          | `services/config/store.h`                                         | `services/config/store.cc`                        |
| `services/shared/*.go` (Go)                          | `services/shared/shared.h`                                        | `services/shared/shared.cc`                       |

Every entry in this table started as a **compilable skeleton** (headers + empty
bodies + `TODO` comments). Many modules have since been promoted to faithful,
unit-tested ports — see the implementation-status section below.

---

# 12. Implementation status (living section)

The port is developed module-by-module. Each module is in one of three states:

- **Full** — logic ported 1:1 from the Rust source and covered by unit tests
  (often ported straight from the Rust `#[cfg(test)]` modules).
- **Gated** — the real implementation needs an external stack or privileged
  host capability that cannot be faked in a plain build, so it is compiled
  behind a `AGENTENV_WITH_*` option and otherwise returns a clear error. This
  matches the Rust behavior when the corresponding crate/feature is absent.
- **Skeleton** — headers + stub bodies, awaiting a future pass.

## 12.1 Fully implemented + tested

| Area | C++ location | Notes / test |
|---|---|---|
| core `Expected/Optional/Identity/Json/Config/MpscQueue` | `src/core/`, `include/agentenv/core/` | `test_expected/identity/json/config/mpsc_queue` |
| core **`NodeIdentity`** (`from_config`: node_id→hostname fallback via `$HOSTNAME`→`/proc/sys/kernel/hostname`→`/etc/hostname`, `cluster_id` parse-or-nil+warn, `service_instance_id` config-or-fresh-v7, build-time `commit`/`version`) — ported 1:1 from Rust `src/identity.rs` incl. its `#[cfg(test)]` | `src/core/identity.cc` | `test_identity` (`node_identity_*`) |
| api DTOs | `src/api/dto.cc` | `test_dto` |
| orchestrator store + metrics | `src/orchestrator/{store,metrics}.cc` | `test_store`, `test_orchestrator` |
| orchestrator **types** (`SandboxState` + `Display`, `SandboxLifecycleEvent(Type)`, `SandboxOperation`, `OrchestratorError` incl. all `#[error(...)]` messages) — 1:1 from Rust `orchestrator/types.rs`+`mod.rs`; `SandboxResources` (Rust `types::SandboxResources`) added in `sandbox/types.h`. NOTE: legacy `LifecyclePhase`/`Sandbox` kept transitional until `Orchestrator` service is aligned. | `src/orchestrator/types.cc`, `include/agentenv/sandbox/types.h` | `test_orchestrator_types` |
| image reference parser | `src/image/reference.cc` | `test_image_reference` |
| image **OCI manifest/layer classification** (`classify_layer/classify_manifest`, overlaybd native/turbo/tar-wrapped) | `src/image/oci_image.cc` | `test_image_oci` |
| image **`commit_index`** (CommitIndex JSON sidecar, `digest_slug`/`commit_dir`/`commit_file`, `seed_commit_file[_trusted_descriptor]`, `index_path`) + self-contained **SHA-256** in `core::Digest` | `src/image/commit_index.cc`, `src/core/digest.cc` | `test_image_commit_index` |
| image **`cache`** (graph/gc pure subset): validated `HardCommitId`/`ImageCacheConfigId`/`ImageCacheHoldOwner`, metadata-store key encoding (hex + `/`-joined), `is_regular_config_filename`, store-free `plan_capacity_eviction` LRU planner, GC report/`summary_from_report` | `src/image/cache.cc` | `test_image_cache` |
| **storage/util** `AlignedBuffer/ReloadableIDAllocator/MMapRegion/CompactWriter` | `storage/util/` | `test_storage_util` |
| **storage/overlaybd LSMT** index (`Segment/SegmentMapping/ReadOnlyIndex/MutableIndex/ComboIndex`), `compress_raw_index`, `DiskSegmentMapping` packing, `HeaderTrailer`, **`OpenIndexFile` (read) + `CompactTo` (write)** | `storage/overlaybd/lsmt.cc` | `test_lsmt`, `test_storage_overlaybd` |
| ublk caps constants + overlaybd config validation | `storage/ublk/caps.h`, `storage/overlaybd/config.cc` | `test_storage_overlaybd` |
| services/scheduler placement (`RoundRobin/Random`, resource filter, binding store, node registry) | `services/scheduler/{strategy,filter,store,node_registry}.cc` | `test_scheduler` |
| services/gateway host routing + schedule hints | `services/gateway/{host_route,schedule_hint}.cc` | `test_gateway` |
| snapshot/repository (errors, interfaces, in-memory backend, list-filter) | `src/snapshot/repository/` | `test_snapshot_repository` |
| **sandbox `process`** (fork/exec + pipe capture + timeout SIGKILL) | `src/sandbox/process.cc` | `test_sandbox_process` |
| **sandbox `network`** `Ipv4Cidr` + `NetworkAddressPlan` (slot/vm/tap IPs) | `src/sandbox/network.cc` | `test_sandbox_process` |
| **sandbox `envd`** `EnvdInstance::WaitForReady` (deadline probe loop) | `src/sandbox/envd.cc` | `test_sandbox_envd` |
| **sandbox `custom_extension`** `SandboxInstanceId` + RAII `HookGuard` | `src/sandbox/custom_extension.cc` | `test_sandbox_envd` |
| **firecracker `config`** boot args, extra-drive-set validation, serial dir, common config validation | `src/sandbox/firecracker/config.cc` | `test_sandbox_firecracker` |
| **firecracker `socket`** real HTTP/1.1-over-AF_UNIX client | `src/sandbox/firecracker/socket.cc` | `test_sandbox_firecracker` |
| **firecracker `mmds`** e2b-field JSON + self-contained SHA-512 | `src/sandbox/firecracker/mmds.cc` | `test_sandbox_fc_pool` |
| **firecracker `pool`** watermark capacity management on `WarmPool<T>` | `src/sandbox/firecracker/pool.cc` | `test_sandbox_fc_pool` |
| **firecracker `instance`** process lifecycle (spawn/wait_for_ready/stop/Drop), log/path/level helpers, API PUTs, `process_vm_readv` reader | `src/sandbox/firecracker/instance.cc` | `test_sandbox_fc_instance` |
| firecracker `sandbox` (FirecrackerBackend) pre-boot validation + jailer paths | `src/sandbox/firecracker/sandbox.cc` | `test_sandbox_firecracker` |
| **p2p** `types/error/config`, `MockTransport` (publish/lookup/fetch/byte-range chunks/unpublish), `discovery` hint-merge, `SchedulerPeerDiscovery` filter+cache | `src/p2p/{types,error,config,mock,discovery}.cc` | `test_p2p` |
| crates `shell-util` `ShellQuote`, `warm-pool` `WarmPool<T>` | `crates/shell-util/shell.cc`, `include/.../warm-pool/pool.h` | `test_shell_util`, `test_warm_pool` |

## 12.2 Gated (needs external stack / privilege)

| Area | Gate | Behavior without it |
|---|---|---|
| firecracker live microVM launch (`FirecrackerBackend::Boot` final step, `Instance` boot/pause/resume via a running VM) | `/dev/kvm` + firecracker binary | pre-boot checks run; launch returns a precise error |
| p2p `iroh` transport (`IrohBlobsP2pTransport`) | `AGENTENV_WITH_IROH` | `Start` returns null; every op returns `P2pError::Disabled` |
| scheduler peer discovery gRPC transport | `AGENTENV_WITH_GRPC` | injectable wire callbacks drive the (tested) cache/filter logic |
| sandbox `network` rtnetlink/iptables side effects | root + netlink | address planning is real; mutation is a no-op TODO |
| sandbox `ublk` data plane | kernel ublk + `AGENTENV_WITH_LIBURING` | ABC + factory placeholders |
| storage/util `io_ring` | `AGENTENV_WITH_LIBURING` | `SpawnIoRingWorker` returns null |

## 12.3 Test suite

`ctest` currently registers **24 test binaries, all passing**, built with
GCC 4.8.5 under `-std=c++11 -Wall -Wextra -Wpedantic`. The micro-test framework
(`tests/microtest.h`) is intentionally dependency-free; migrating to GoogleTest
is a drop-in change.

Build & test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```
