// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/{common,posixfs,oss}/ — concrete
// SnapshotRepository backends.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/cfg.h"
// `OverlaybdLayerStore` — the pair factory below has to keep the store the
// resolver was built over alive, so the struct names its type.
#include "agentenv/image/cache_layer_store.h"
#include "agentenv/snapshot/artifact_cache.h"
#include "agentenv/snapshot/repository/interfaces.h"
// `SnapshotRuntimeResolver` — the other half of `SnapshotBackend`.
#include "agentenv/snapshot/resolver.h"

namespace agentenv {
namespace snapshot {
namespace repository {

namespace oss {
class OssClient;
}  // namespace oss

/// Rust: backends/common/ — an in-memory SnapshotRepository used as the base
/// for tests and as the reference implementation of the trait semantics.
class InMemorySnapshotRepository : public SnapshotRepository {
 public:
    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord& record) override;
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string& id_or_alias) override;
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter& filter) override;
    RepositoryResult<core::Unit> Delete(const std::string& id_or_alias) override;
    RepositoryResult<core::Optional<core::SnapshotId> >
        ResolveAlias(const std::string& alias) override;
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId& id) override;
    RepositoryResult<core::Unit>
        MarkBuildError(const core::SnapshotId& id,
                       const TemplateBuildErrorReason& reason) override;
 private:
    core::Optional<SnapshotRecord> FindLocked(const std::string& id_or_alias) const;
    mutable std::mutex mu_;
    std::map<std::string, SnapshotRecord> by_id_;
    std::map<std::string, std::string>    alias_to_id_;
};

// Rust: backends/posixfs/ — construct `posixfs::PosixFsSnapshotRepository`
// directly (see `posixfs/backend.h`). There is deliberately no
// `MakePosixFsRepository(root_dir)` factory: the backend needs a catalog store
// *and* an artifact store, and the one that used to be declared here took only
// a root, ignored it, and handed back an in-memory stub.

/// Rust `OssRepository::from_config` — an object-storage repository.
///
/// Takes the configuration section rather than a URL, as upstream does: a
/// bucket URL alone cannot carry the endpoint, region and credentials, so the
/// previous signature could never have constructed one.
///
/// Fails rather than degrading: an incomplete `[backend.oss]` section, a
/// `credential_process`, or a build without libcurl are all reported here
/// instead of at the first request. The returned repository serves documents
/// and blobs; its snapshot *record* methods report `Unsupported` (see
/// `oss/repository.h`).
///
/// `publish_compression` is `AppConfig.snapshot.publish_compression`, and is
/// **required** rather than defaulted, matching Rust's `from_config`. It is
/// the switch that decides whether published layers are recontainerized as
/// zfile, so a caller that does not name it has not decided what this
/// repository uploads -- and the safe-looking default (the config struct's
/// own, which has `enabled = true`) would turn compression *on* for a caller
/// that said nothing. The `OssSnapshotRepository` constructor does default
/// its policy, because there the fallback is `PublishPolicy()`, which is
/// passthrough.
/// `client`, when non-null, also receives the `OssClient` the repository was
/// built over. Added for `SnapshotImageService`, which has to read managed
/// layer objects *directly* — it downloads them to feed `regctl blob put`,
/// which the repository interface has no operation for. It must be the **same
/// client**, not a second one built from the same config: two would each
/// carry their own credential refresh and connection state, so a token
/// rotation observed by one would leave the other signing with a stale one.
/// `image_storage` is the other half of the policy, and is **required** for
/// the same reason `publish_compression` is: it decides whether a published
/// disk image goes to its source registry or becomes a managed layer in this
/// bucket, so a caller that has not named it has not decided what this
/// repository publishes. See `SnapshotImageStorageFor`.
RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeOssRepository(
    const cfg::OssBackendConfig& config,
    const cfg::SnapshotPublishCompressionConfig& publish_compression,
    cfg::SnapshotImageStoragePolicy image_storage,
    std::shared_ptr<oss::OssClient>* client = 0);

/// Rust `snapshot/backends/mod.rs:90-94` — the image-storage policy a node's
/// config asks for.
///
/// `snapshot.image_publish.enabled` is the operator's switch, and this is the
/// whole of its meaning. Exposed rather than inlined into
/// `MakeRepositoryFromConfig` because it is the one *decision* in that branch
/// and is otherwise only reachable by constructing a live OSS client, which a
/// test cannot do without a bucket.
///
/// **Reading it does not mean a node honours it.** Only the OSS backend
/// carries the policy, so on posix-fs this value is computed and then
/// discarded — which is upstream's shape too (`mod.rs` derives it inside the
/// `Oss` arm alone). An operator who sets `image_publish = true` on a
/// posix-fs node gets managed layers, here and in Rust.
cfg::SnapshotImageStoragePolicy SnapshotImageStorageFor(const cfg::AppConfig& config);

/// Rust `build_snapshot_backend_from_config` (`snapshot/backends/mod.rs:57`)
/// — the repository the node runs on, selected by
/// `snapshot.repository_backend`.
///
/// **Selected by the declared backend, not by which sections happen to be
/// present.** That distinction is the bug this function exists to fix: the
/// startup path used to branch on `backend.posix_fs.has_value()`, and config
/// normalisation only auto-inserts that section when the backend *is*
/// posix-fs (`cfg.cc:578`). A node declaring `repository_backend = "oss"`
/// that still carried a `[backend.posix_fs]` section -- the ordinary state of
/// a node migrated from posix-fs -- therefore published to its local disk
/// while believing it published to the bucket.
///
/// A missing section for the declared backend is an error, matching Rust's
/// `.context("backend.oss config is required when repository_backend = oss")`.
/// Degrading to the other backend is exactly the failure above, and degrading
/// to no repository would disable volumes on a node whose config asked for
/// one.
///
/// Returns the repository alone. Upstream has no such function -- its
/// `build_snapshot_backend_from_config` returns the repository *and* the
/// runtime resolver -- so this is a narrower entry point kept for the callers
/// that genuinely only publish and read records. Everything that has to
/// *launch* from a snapshot needs `MakeSnapshotBackendFromConfig` below.
///
/// A previous note here claimed that neither the resolver nor the
/// `LocalArtifactCache` it is built over was ported. Both assertions were
/// wrong by the time it was written: `LocalArtifactCache` lives in
/// `snapshot/artifact_cache.h`, and `PosixFsRuntimeResolver` implements the
/// `SnapshotRuntimeResolver` seam in `snapshot/resolver.h`. What was actually
/// missing was this file's counterpart to upstream's *pair* return, which is
/// what left the resolver with no production caller at all.
RepositoryResult<std::shared_ptr<SnapshotRepository> > MakeRepositoryFromConfig(
    const cfg::AppConfig& config);

/// Rust `PosixFsBackend`/`OssBackend::into_parts()` — what
/// `build_snapshot_backend_from_config` returns.
///
/// Upstream returns `(Arc<dyn Repository>, Arc<dyn SnapshotRuntimeResolver>)`.
/// A struct rather than a `std::pair` because two more members have to travel
/// with them, and a pair would have hidden that:
///
///   * `overlaybd_layers` — `RepositoryRuntimeResolver` takes the layer store
///     as a **raw** `const` pointer (so does `RuntimeImageMaterializer`), so
///     the `shared_ptr` that owns it must outlive the resolver. Upstream holds
///     an `Arc` inside the resolver and has nothing to arrange. Dropping this
///     field would leave every resolve reading a destroyed store.
///   * `artifact_cache` — the resolver holds a `shared_ptr` to it, so this is
///     not strictly load-bearing; it is exposed because the cache is the thing
///     that decides when materialised files are reclaimed, and a caller that
///     wants to hold a lease across several launches needs to name it.
struct SnapshotBackend {
    std::shared_ptr<SnapshotRepository>      repository;
    /// Never null now. It used to be null on the OSS backend, because this
    /// port had only a `PosixFsRuntimeResolver` built from a *repository root
    /// path*, which cannot be pointed at a bucket -- so an OSS node got a
    /// repository and no resolver, and every snapshot launch answered 501.
    /// `RepositoryRuntimeResolver` is generic over the repository interface,
    /// as upstream's always was.
    std::shared_ptr<SnapshotRuntimeResolver> runtime_resolver;
    std::shared_ptr<image::cache::OverlaybdLayerStore> overlaybd_layers;
    std::shared_ptr<LocalArtifactCache>      artifact_cache;
    /// The transport this backend was built with, handed back so the API layer
    /// can announce a freshly published snapshot over it.
    ///
    /// Upstream has no equivalent field because its `SnapshotManager` owns
    /// both the repository and the transport, and `publish_with_recording`
    /// reaches the second through `self`. This port's `Publish` is a free
    /// function, so the transport has to travel with the backend to reach
    /// `PublishSnapshotP2pArtifacts` — otherwise `main` would be the only
    /// place holding it and every committed snapshot would go unannounced.
    ///
    /// Null when `[p2p].enabled` is false, which the announcement reads as
    /// "nothing to announce".
    std::shared_ptr<p2p::P2pTransport>       p2p_transport;
};

/// Rust `build_snapshot_backend_from_config` (`snapshot/backends/mod.rs:57`),
/// returning both halves.
///
/// The repository comes from `MakeRepositoryFromConfig`, so the
/// declared-backend selection and its error messages are shared rather than
/// written twice. The resolver is added on top, for posix-fs only.
///
/// Both node-local roots come from `snapshot.local_cache_path`, as upstream's
/// `shared_cache_root` does -- **not** from
/// `posixfs::SharedRuntimeCacheRoot()`. That default exists for
/// `PosixFsBackendConfig`'s optional fields and upstream never reaches it from
/// this path either, because it always passes `cache_root` explicitly here.
/// Letting it apply would put a node's materialised configs under `/tmp`
/// while its config named somewhere else.
///
/// Creating the cache root is a side effect, and an intended one: upstream
/// calls `LocalArtifactCache::new` before building the backend and propagates
/// its failure with `?`. A node whose cache root cannot be created cannot
/// resolve any snapshot, and finding that out at startup is the point.
///
/// `p2p_transport` is the node's transport, which the resolver uses to try a
/// peer before the repository for memory dumps, rootfs images and firecracker
/// manifests. Passing null is legal and means every remote artifact is fetched
/// from the repository — bandwidth, not correctness, which is why upstream
/// treats this leg as best-effort too.
///
/// **`snapshot.p2p_enabled` is applied here, not by the caller.** Upstream
/// gates at the call site (`config.snapshot.p2p_enabled.then(|| ...)`,
/// `server.rs:107`). Moving it inside is a deliberate departure: the switch
/// had no consumer anywhere in this tree for as long as the parameter did not
/// exist, and a gate the caller has to remember is a gate that returns to that
/// state the next time a caller is added. The parameter therefore means "the
/// transport this node has", and whether a *snapshot* resolve may use it stays
/// with the config that decides it.
RepositoryResult<SnapshotBackend> MakeSnapshotBackendFromConfig(
    const cfg::AppConfig& config,
    std::shared_ptr<p2p::P2pTransport> p2p_transport = std::shared_ptr<p2p::P2pTransport>());

}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_BACKENDS_H_
