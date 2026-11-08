// SPDX-License-Identifier: MIT
// Rust: src/snapshot/resolver.rs :: RepositoryRuntimeResolver::resolve,
//       reached through the `SnapshotRuntimeResolver` trait.
//
// Two things here had no coverage at all.
//
// **The seam.** `SnapshotRuntimeResolver` is what upstream's
// `build_snapshot_backend` returns alongside the repository, so it is the
// point a backend is selected at. Its C++ copy used to sit in
// `repository/interfaces.h` declaring `Resolve` as returning
// `RepositoryResult<std::string>` against upstream's `RunnableSnapshot` --
// unsatisfiable, and so never satisfied: `PosixFsRuntimeResolver` had no base
// class and the seam had no implementations. These tests call through a
// `SnapshotRuntimeResolver&`, which is what makes the fix load-bearing rather
// than cosmetic.
//
// **The publish/resolve round trip.** Publishing writes artifacts through
// `PosixFsArtifactStore`; resolving reads them through
// `PosixFsSnapshotArtifactLayout`. Nothing checked that the two agree on
// where the files go, and they are the two halves of one contract -- a
// snapshot is published on one node and resolved on another. A layout change
// on either side would previously have been caught by neither.
#include "microtest.h"

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/publish.h"
#include "agentenv/snapshot/repository/backends.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "agentenv/snapshot/repository/posixfs/runtime.h"
#include "agentenv/snapshot/resolver.h"

using namespace agentenv;  // NOLINT

namespace posixfs = agentenv::snapshot::repository::posixfs;
namespace repo = agentenv::snapshot::repository;

namespace {

struct TempTree {
    std::string path;
    TempTree() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("agentenv-runtime-resolver-");
        MT_EXPECT_TRUE(made.ok());
        if (made.ok()) path = made.value();
    }
    ~TempTree() {
        if (!path.empty()) core::fs::RemoveDirAll(path);
    }
    std::string At(const std::string& name) const { return core::fs::Join(path, name); }
};

/// Records whether it is asked anything. On a posix repository it should not
/// be: managed layers are read in place and arrive already bound to `file=`.
/// It is still required as a constructor argument, so it stands in and
/// reports.
class FileBindingLayerStore : public image::cache::OverlaybdLayerStore {
 public:
    image::cache::OverlaybdLayerLocation LayerLocation(const std::string& digest, uint64_t,
                                                       bool) const override {
        ++calls;
        return image::cache::OverlaybdLayerLocation::LocalFile("/not-used/" + digest);
    }
    std::vector<std::string> PublishableRoots() const override {
        return std::vector<std::string>();
    }
    mutable int calls = 0;
};

/// An overlaybd image config with one local lower whose descriptor matches the
/// file, so `StoreDiskLayers` trusts it instead of hashing.
void WriteOneLayerConfig(const std::string& config_path, const std::string& layer_path,
                         const std::string& bytes) {
    MT_EXPECT_TRUE(core::fs::Write(layer_path, bytes).ok());
    storage::overlaybd::ImageConfig config;
    storage::overlaybd::LayerConfig lower;
    lower.file   = layer_path;
    lower.digest = core::Sha256Digest(bytes);
    lower.size   = static_cast<uint64_t>(bytes.size());
    config.lowers.push_back(lower);
    MT_EXPECT_TRUE(storage::overlaybd::SaveImageConfig(config_path, config).ok());
}

/// A capture on disk, shaped the way `FirecrackerSandboxBackend` leaves one.
struct Capture {
    std::string dir;
    sandbox::SandboxSnapshotManifest manifest;

    explicit Capture(const std::string& d) : dir(d) {
        MT_EXPECT_TRUE(core::fs::CreateDirAll(core::fs::Join(dir, "rootfs")).ok());
        MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(dir, "vm_state.bin"), "vm state").ok());
        WriteOneLayerConfig(core::fs::Join(dir, "mem_image.json"),
                            core::fs::Join(dir, "mem.commit"), "memory layer bytes");
        WriteOneLayerConfig(core::fs::Join(dir, "rootfs/image.json"),
                            core::fs::Join(dir, "rootfs/base.commit"), "rootfs layer bytes");

        const core::Expected<sandbox::SandboxSnapshotManifest, std::string> made =
            sandbox::SandboxSnapshotManifest::New(
                sandbox::kFirecrackerBackend, core::fs::Join(dir, "vm_state.bin"),
                core::fs::Join(dir, "mem_image.json"), 512ULL * 1024 * 1024,
                core::fs::Join(dir, "rootfs/image.json"), 1024ULL * 1024 * 1024,
                std::vector<sandbox::ExtraDrive>());
        MT_EXPECT_TRUE(made.ok());
        if (made.ok()) manifest = made.value();
    }
};

snapshot::SnapshotPublishMetadata PublishMetadata() {
    snapshot::SnapshotPublishMetadata publish;
    publish.id = core::SnapshotId::Fresh();
    publish.source = snapshot::SnapshotPublishSource::Sandbox(
        core::SandboxId::Fresh().ToString(),
        core::Optional<std::map<std::string, std::string> >());
    publish.resources.cpu_count    = 2;
    publish.resources.memory_mib   = 512;
    publish.resources.disk_size_mib = 1024;
    publish.runtime_versions.tools_drive_version = "1.2.3";
    return publish;
}

/// Publishes a capture into a fresh posix repository and hands back the
/// committed record plus a resolver pointed at the same root.
struct Fixture {
    TempTree tree;
    std::shared_ptr<posixfs::PosixFsSnapshotRepository> repository;
    std::shared_ptr<snapshot::LocalArtifactCache> cache;
    FileBindingLayerStore store;
    std::unique_ptr<posixfs::PosixFsRuntimeResolver> resolver;
    snapshot::SnapshotRecord record;
    bool ready = false;

    Fixture() {
        const std::string root = tree.At("repository");
        repository = std::make_shared<posixfs::PosixFsSnapshotRepository>(
            std::make_shared<posixfs::PosixFsCatalogStore>(root),
            std::make_shared<posixfs::PosixFsArtifactStore>(root));

        Capture capture(tree.At("capture"));
        const repo::RepositoryResult<snapshot::SnapshotPublication> published =
            snapshot::Publish(*repository, PublishMetadata(), capture.manifest);
        MT_EXPECT_TRUE(published.ok());
        if (!published.ok()) {
            MT_EXPECT_EQ(published.error().ToString(), std::string(""));
            return;
        }
        record = published.value().record;

        const core::Expected<std::shared_ptr<snapshot::LocalArtifactCache>, std::string> made =
            snapshot::LocalArtifactCache::New(tree.At("cache"), false, 0);
        MT_EXPECT_TRUE(made.ok());
        if (!made.ok()) return;        cache = made.value();

        resolver.reset(new posixfs::PosixFsRuntimeResolver(root, tree.At("runtime"), &store,
                                                           cache));
        ready = true;
    }

    /// Publishes a second, independent capture into the same repository.
    /// Needed because the per-snapshot isolation of the runtime cache cannot
    /// be observed with only one snapshot in play.
    snapshot::SnapshotRecord PublishAnother(const std::string& capture_name) {
        Capture capture(tree.At(capture_name));
        const repo::RepositoryResult<snapshot::SnapshotPublication> published =
            snapshot::Publish(*repository, PublishMetadata(), capture.manifest);
        MT_EXPECT_TRUE(published.ok());
        if (!published.ok()) {
            MT_EXPECT_EQ(published.error().ToString(), std::string(""));
            return snapshot::SnapshotRecord();
        }
        return published.value().record;
    }
};

}  // namespace

MT_TEST(a_published_snapshot_resolves_through_the_runtime_resolver_seam) {
    Fixture f;
    if (!f.ready) return;

    // Called through the trait, not the concrete class: this is the seam a
    // backend selector hands back, and it could not be satisfied at all
    // before `Resolve` returned a `RunnableSnapshot`.
    snapshot::SnapshotRuntimeResolver& seam = *f.resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> runnable = seam.Resolve(f.record);
    MT_EXPECT_TRUE(runnable.ok());
    if (!runnable.ok()) {
        MT_EXPECT_EQ(runnable.error().ToString(), std::string(""));
        return;
    }

    // The manifest now names node-local paths, not the ones the capture had.
    // `vm_state` is read in place from the repository; the two image configs
    // are materialised under the runtime root.
    const sandbox::SandboxSnapshotManifest& manifest = runnable.value().manifest;
    MT_EXPECT_TRUE(core::fs::Exists(manifest.vm_state.path));
    MT_EXPECT_TRUE(core::fs::Exists(manifest.memory.image_config_path));
    MT_EXPECT_TRUE(core::fs::Exists(manifest.rootfs.image_config_path));
    MT_EXPECT_TRUE(manifest.memory.image_config_path.find(f.tree.At("runtime")) == 0);
    MT_EXPECT_TRUE(manifest.rootfs.image_config_path.find(f.tree.At("runtime")) == 0);

    // The round trip is the point: `vm_state` sits where the *artifact store*
    // put it, read back through the *layout*. If those two disagreed the
    // resolve would have failed with ArtifactNotFound.
    MT_EXPECT_EQ(manifest.vm_state.path,
                 posixfs::PosixFsSnapshotArtifactLayout(f.tree.At("repository"), f.record.id)
                     .Path(snapshot::kSnapshotArtifactLayout.vm_state));

    // The layer store is **not** consulted, and that is the posix contract
    // rather than an omission: `ResolveLocalManagedLayer` already binds each
    // managed lower to its path inside the repository, so
    // `AttachLocalLayerLocation` sees a placement and leaves it alone.
    // Upstream says the same thing where it builds this resolver -- "files
    // are read in place, so the resolver never fetches from peers".
    //
    // An earlier version of this test asserted the opposite (`calls > 0`) and
    // failed; the production code was right. The store's `file=`/`dir=`
    // decision governs *external* layers and the cache-backed path, which is
    // covered in `test_image_cache_service.cc`.
    MT_EXPECT_EQ(f.store.calls, 0);

    // ...and the lowers really do point into the repository, which is what
    // "read in place" has to mean for the launch to find them.
    const core::Expected<storage::overlaybd::ImageConfig, std::string> rootfs_config =
        storage::overlaybd::LoadImageConfig(manifest.rootfs.image_config_path);
    MT_EXPECT_TRUE(rootfs_config.ok());
    if (rootfs_config.ok()) {
        MT_EXPECT_TRUE(!rootfs_config.value().lowers.empty());
        if (!rootfs_config.value().lowers.empty()) {
            // Normalized, because `LoadImageConfig` is read +
            // `ResolveImageConfigLocalPaths` + validate: it rebases local
            // paths and collapses a `//` that the fixture's `TMPDIR` carries.
            // The two `find(...) == 0` checks above compare paths the
            // materialiser built directly, which never went through that.
            const std::string repository_prefix =
                storage::overlaybd::LexicallyNormalizePath(f.tree.At("repository"));
            MT_EXPECT_TRUE(!rootfs_config.value().lowers[0].file.empty());
            MT_EXPECT_TRUE(rootfs_config.value().lowers[0].file.find(repository_prefix) == 0);
            MT_EXPECT_TRUE(core::fs::Exists(rootfs_config.value().lowers[0].file));
        }
    }

    // The lease is held: it is what keeps the materialised configs from being
    // reclaimed while the launch that needs them is still starting.
    MT_EXPECT_TRUE(static_cast<bool>(runnable.value().lease));

    // Carried through unchanged, because the launch reads its resources from
    // the record rather than the manifest.
    MT_EXPECT_EQ(runnable.value().record.id.ToString(), f.record.id.ToString());
    MT_EXPECT_EQ(runnable.value().record.resources.cpu_count, static_cast<uint32_t>(2));
}

MT_TEST(resolving_twice_reuses_the_materialized_configs) {
    // The runtime root is a cache, so a second resolve of one snapshot must
    // land on the same files. Fresh paths per resolve would make every launch
    // re-materialise and would defeat the lease, since the files a previous
    // lease pinned would no longer be the ones in use.
    Fixture f;
    if (!f.ready) return;

    snapshot::SnapshotRuntimeResolver& seam = *f.resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> first = seam.Resolve(f.record);
    const repo::RepositoryResult<snapshot::RunnableSnapshot> second = seam.Resolve(f.record);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());
    if (!first.ok() || !second.ok()) return;

    MT_EXPECT_EQ(first.value().manifest.rootfs.image_config_path,
                 second.value().manifest.rootfs.image_config_path);
    MT_EXPECT_EQ(first.value().manifest.memory.image_config_path,
                 second.value().manifest.memory.image_config_path);
}

MT_TEST(two_snapshots_get_separate_materialized_configs) {
    // The runtime cache is keyed per snapshot. Sharing one path would make
    // the second resolve overwrite the first's `image.json` while the first
    // lease still pins it -- so a sandbox launched from snapshot A would come
    // up on snapshot B's rootfs, with nothing having failed.
    //
    // Added because a mutation that dropped the snapshot id from the runtime
    // path **survived** the rest of this file: with one snapshot resolved,
    // per-snapshot isolation is invisible.
    Fixture f;
    if (!f.ready) return;

    const snapshot::SnapshotRecord other = f.PublishAnother("capture-2");
    if (other.id.ToString() == core::SnapshotId().ToString()) return;
    MT_EXPECT_TRUE(other.id.ToString() != f.record.id.ToString());

    snapshot::SnapshotRuntimeResolver& seam = *f.resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> first = seam.Resolve(f.record);
    const repo::RepositoryResult<snapshot::RunnableSnapshot> second = seam.Resolve(other);
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());
    if (!first.ok() || !second.ok()) return;

    MT_EXPECT_TRUE(first.value().manifest.rootfs.image_config_path !=
                   second.value().manifest.rootfs.image_config_path);
    MT_EXPECT_TRUE(first.value().manifest.memory.image_config_path !=
                   second.value().manifest.memory.image_config_path);
    // Both still present: the second resolve must not have replaced the
    // first's files, which is the failure the shared path would cause.
    MT_EXPECT_TRUE(core::fs::Exists(first.value().manifest.rootfs.image_config_path));
    MT_EXPECT_TRUE(core::fs::Exists(second.value().manifest.rootfs.image_config_path));
    // And each names its own vm_state in the repository.
    MT_EXPECT_TRUE(first.value().manifest.vm_state.path !=
                   second.value().manifest.vm_state.path);
}

MT_TEST(an_uncommitted_record_is_refused_as_not_ready) {
    // A record exists from the moment a template build is queued, so an
    // absent `committed` payload is an ordinary state rather than corruption.
    // Resolving one would produce a manifest naming artifacts no build has
    // written yet.
    Fixture f;
    if (!f.ready) return;

    snapshot::SnapshotRecord pending = f.record;
    pending.committed = core::Optional<snapshot::CommittedSnapshot>();

    snapshot::SnapshotRuntimeResolver& seam = *f.resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> runnable = seam.Resolve(pending);
    MT_EXPECT_TRUE(!runnable.ok());
    if (runnable.ok()) return;
    MT_EXPECT_TRUE(runnable.error().kind == repo::RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(runnable.error().ToString().find("not ready") != std::string::npos);
}

MT_TEST(a_missing_vm_state_is_reported_as_a_missing_artifact) {
    // Distinguished from "not ready": the record promises a capture that the
    // repository does not have, which is a different operator problem --
    // ArtifactNotFound is also what tells a caller to look at another node
    // rather than at the build.
    Fixture f;
    if (!f.ready) return;

    const std::string vm_state =
        posixfs::PosixFsSnapshotArtifactLayout(f.tree.At("repository"), f.record.id)
            .Path(snapshot::kSnapshotArtifactLayout.vm_state);
    MT_EXPECT_TRUE(core::fs::Exists(vm_state));
    MT_EXPECT_TRUE(core::fs::RemoveFile(vm_state).ok());

    snapshot::SnapshotRuntimeResolver& seam = *f.resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> runnable = seam.Resolve(f.record);
    MT_EXPECT_TRUE(!runnable.ok());
    if (runnable.ok()) return;
    MT_EXPECT_TRUE(runnable.error().kind == repo::RepositoryErrorKind::ArtifactNotFound);
}

// ---- the backend pair ------------------------------------------------------
//
// Rust `build_snapshot_backend_from_config` returns the repository *and* the
// resolver. The C++ side returned the repository alone, which is why the
// resolver above -- fully implemented, now covered -- had zero production
// callers: nothing handed one out.

MT_TEST(the_posixfs_backend_pair_carries_a_resolver_and_its_store) {
    TempTree tree;
    if (tree.path.empty()) return;

    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::PosixFs;
    config.snapshot.local_cache_path   = tree.At("cache");
    cfg::PosixFsBackendConfig posix;
    posix.snapshot_store = tree.At("store");
    config.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>(posix);

    const repo::RepositoryResult<repo::SnapshotBackend> made =
        repo::MakeSnapshotBackendFromConfig(config);
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;

    MT_EXPECT_TRUE(made.value().repository.get() != NULL);
    MT_EXPECT_TRUE(made.value().runtime_resolver.get() != NULL);
    // The store and cache travel with the pair. The resolver borrows the store
    // as a raw pointer, so a pair that dropped it would hand back a resolver
    // reading freed memory -- and nothing about the resolver's own interface
    // would reveal that.
    MT_EXPECT_TRUE(made.value().overlaybd_layers.get() != NULL);
    MT_EXPECT_TRUE(made.value().artifact_cache.get() != NULL);
    // Rust creates the cache root before building the backend, and propagates
    // the failure. A node whose cache root cannot be made cannot resolve.
    MT_EXPECT_TRUE(core::fs::Exists(tree.At("cache")));
}

MT_TEST(the_backend_pair_resolves_against_the_configured_repository_root) {
    // The repository and the resolver must agree on the root, or every resolve
    // reports ArtifactNotFound against files that exist. Checked by publishing
    // through the pair's repository and resolving through the pair's resolver.
    TempTree tree;
    if (tree.path.empty()) return;

    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::PosixFs;
    config.snapshot.local_cache_path   = tree.At("cache");
    cfg::PosixFsBackendConfig posix;
    posix.snapshot_store = tree.At("store");
    config.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>(posix);

    const repo::RepositoryResult<repo::SnapshotBackend> made =
        repo::MakeSnapshotBackendFromConfig(config);
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;

    Capture capture(tree.At("pair-capture"));
    const repo::RepositoryResult<snapshot::SnapshotPublication> published =
        snapshot::Publish(*made.value().repository, PublishMetadata(), capture.manifest);
    MT_EXPECT_TRUE(published.ok());
    if (!published.ok()) {
        MT_EXPECT_EQ(published.error().ToString(), std::string(""));
        return;
    }

    snapshot::SnapshotRuntimeResolver& seam = *made.value().runtime_resolver;
    const repo::RepositoryResult<snapshot::RunnableSnapshot> runnable =
        seam.Resolve(published.value().record);
    MT_EXPECT_TRUE(runnable.ok());
    if (!runnable.ok()) {
        MT_EXPECT_EQ(runnable.error().ToString(), std::string(""));
        return;
    }
    MT_EXPECT_TRUE(core::fs::Exists(runnable.value().manifest.rootfs.image_config_path));
    // Materialised under the configured cache root, not under the `/tmp`
    // default `SharedRuntimeCacheRoot()` would have supplied.
    MT_EXPECT_TRUE(runnable.value().manifest.rootfs.image_config_path.find(
                       tree.At("cache")) == 0);
}

MT_TEST(a_posixfs_backend_without_its_section_is_refused) {
    // Not defaulted: guessing a snapshot store root would put a node's layers
    // somewhere nobody chose.
    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::PosixFs;
    config.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>();

    const repo::RepositoryResult<repo::SnapshotBackend> made =
        repo::MakeSnapshotBackendFromConfig(config);
    MT_EXPECT_TRUE(!made.ok());
    if (made.ok()) return;
    MT_EXPECT_TRUE(made.error().kind == repo::RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(made.error().ToString().find("backend.posix_fs") != std::string::npos);
}

MT_MAIN
