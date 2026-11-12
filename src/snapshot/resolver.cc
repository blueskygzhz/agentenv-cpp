// SPDX-License-Identifier: MIT
// Rust: src/snapshot/resolver.rs :: RepositoryRuntimeResolver.
//
// Upstream's opening sentence is the design: "One resolver serves every
// repository: files a backend can read in place are used where they are;
// everything else is fetched, P2P-first, into the node-local cache. A missing
// in-place file is an error, never a reason to fetch it from peers."
//
// This file replaces a `PosixFsRuntimeResolver` that composed a repository
// *root path* with the posix-fs layout to find every artifact. See the class
// comment in `snapshot/resolver.h` for what that cost: an OSS node had no
// resolver and answered 501 to every snapshot launch, and
// `SnapshotRepository::FetchBlob` -- the operation whose entire purpose is
// reading a repository this node cannot mount -- had no production caller.
#include "agentenv/snapshot/resolver.h"

#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/snapshot/p2p.h"

namespace agentenv {
namespace snapshot {

namespace repo = repository;

using repo::BlobKey;
using repo::BlobLocation;
using repo::LayerRole;
using repo::RepositoryError;
using repo::RepositoryResult;
using repo::SnapshotFile;
using repo::SnapshotFileName;

namespace {

/// Rust's `LAYER_EXISTS_CONCURRENCY`.
///
/// **Deviation**: upstream checks the layers through
/// `buffer_unordered(LAYER_EXISTS_CONCURRENCY)`, 16 at a time. This runs them
/// sequentially, for the reason recorded for the metrics sweep: there is no
/// task runtime here, and a thread pool would be a second concurrency
/// mechanism in a tree that has none.
///
/// The constant is kept because it is the *semantics* that survive, not the
/// speed: every managed layer is checked, and the first missing one is the
/// error. A snapshot with many layers resolves more slowly on a repository
/// whose `HasBlob` is a network round trip -- an OSS node pays
/// `layers x latency` where upstream pays `ceil(layers/16) x latency`.
const std::size_t kLayerExistsConcurrency = 16;

/// Rust `validate_managed_layers` — every managed layer must be stored before
/// a config that references it is written.
///
/// Checking first rather than letting the launch fail later is the point: a
/// written `image.json` naming a layer that is not in the repository produces
/// an overlaybd mount error at boot, far from the missing digest, and the
/// cache would happily pin that config for reuse.
RepositoryResult<core::Unit> ValidateManagedLayers(
    repo::SnapshotRepository& repository,
    const std::vector<OverlaybdLayerRef>& layers,
    const std::string& label, LayerRole role) {
    for (std::size_t index = 0; index < layers.size(); ++index) {
        if (layers[index].kind != OverlaybdLayerRef::Kind::Managed) continue;
        const std::string& digest = layers[index].managed.digest;
        const RepositoryResult<bool> exists =
            repository.HasBlob(BlobKey::Layer(digest, role));
        if (!exists.ok()) return core::make_unexpected(exists.error());
        if (!exists.value()) {
            std::ostringstream os;
            // Upstream's message, including the layer's index within the set:
            // the digest alone does not say *which* layer of a rootfs chain
            // is gone, and the index is what makes a partially published
            // snapshot diagnosable.
            os << label << ": managed layer " << index << " '" << digest << "' is missing";
            return core::make_unexpected(RepositoryError::ArtifactNotFound(os.str()));
        }
    }
    return core::Unit();
}

/// Bridges `RuntimeImageMaterializer::ResolveManagedFn` (a plain function
/// pointer plus a `void*`) onto the resolver member function.
struct ResolveContext {
    const RepositoryRuntimeResolver* resolver;
    LayerRole                        role;
};

RuntimeImageMaterializer::ResolveManagedResult ResolveManagedCallback(
    void* ctx, size_t /*index*/, const ManagedLayer& layer) {
    const ResolveContext* rc = static_cast<const ResolveContext*>(ctx);
    return rc->resolver->ResolveManagedLayer(layer, rc->role);
}

}  // namespace

RepositoryRuntimeResolver::RepositoryRuntimeResolver(
    std::shared_ptr<repo::SnapshotRepository> repository,
    std::shared_ptr<LocalArtifactCache> cache,
    const std::string& runtime_root,
    const image::cache::OverlaybdLayerStore* store,
    std::shared_ptr<p2p::P2pTransport> p2p_transport)
    : repository_(repository),
      cache_(cache),
      image_materializer_(runtime_root, store),
      p2p_transport_(p2p_transport) {}

// ---- individual artifacts ---------------------------------------------------

RepositoryResult<storage::overlaybd::LayerConfig>
RepositoryRuntimeResolver::ResolveManagedLayer(const ManagedLayer& layer,
                                               LayerRole role) const {
    // The whole generalisation, in one call. The posix-only predecessor built
    // a path from its repository root and demanded the file exist there;
    // `ManagedLayerConfig` asks the repository instead, so a `Local` answer
    // still reads in place and a `Remote` one leaves `file` empty for
    // overlaybd to address by digest against `repoBlobUrl`.
    return repo::ManagedLayerConfig(*repository_, layer, role);
}

RepositoryResult<std::string>
RepositoryRuntimeResolver::VmStatePath(const core::SnapshotId& id,
                                       CacheArtifactLease* lease) {
    const BlobKey key = BlobKey::Snapshot(id, SnapshotFile::VmState);
    const std::string name = SnapshotFileName(SnapshotFile::VmState);

    const RepositoryResult<BlobLocation> location = repository_->LocateBlob(key);
    if (!location.ok()) return core::make_unexpected(location.error());

    if (location.value().kind == BlobLocation::Kind::Local) {
        const std::string& path = location.value().value;
        // Missing in place is an **error**, not a reason to try peers. The
        // repository claims to hold these bytes at this path; a node that
        // cannot see them there is misconfigured, and quietly sourcing them
        // from a peer would mask that for as long as some peer had a copy.
        if (!core::fs::Exists(path)) {
            return core::make_unexpected(
                RepositoryError::ArtifactNotFound(std::string("vm state at ") + path));
        }
        return path;
    }

    // Remote: fetch into the node-local cache, peers first. The cache keys on
    // the artifact rather than the destination path, so two concurrent
    // resolves of one snapshot download it once.
    struct Fetch {
        repo::SnapshotRepository*          repository;
        const BlobKey*                     key;
        std::shared_ptr<p2p::P2pTransport> transport;
        p2p::P2pArtifactKey                p2p_key;

        core::Expected<uint64_t, std::string> operator()(const std::string& dest) const {
            if (transport) {
                const core::Expected<uint64_t, std::string> fetched =
                    FetchArtifact(transport, p2p_key, dest);
                if (fetched.ok()) return fetched.value();
                // Best-effort, exactly as upstream: a peer that cannot serve
                // the artifact must not fail the launch while the repository
                // still holds it. Logged at debug because on a node with no
                // seeded peers this is the normal path, and a warning per
                // resume would train an operator to ignore it.
                AGENTENV_DEBUG("P2P vm_state fetch failed; using repository fallback: "
                               << fetched.error());
            }
            // The one production call to `FetchBlob`. Without this branch the
            // method had no caller at all, which is why it looked like a
            // speculative capability rather than the thing that makes a
            // non-shared repository readable.
            const RepositoryResult<uint64_t> copied = repository->FetchBlob(*key, dest);
            if (!copied.ok()) return core::make_unexpected(copied.error().ToString());
            return copied.value();
        }
    };

    Fetch fetch;
    fetch.repository = repository_.get();
    fetch.key        = &key;
    fetch.transport  = p2p_transport_;
    fetch.p2p_key    = FixedArtifactKey(id, name);

    const core::Expected<CacheHandle*, std::string> handle =
        cache_->EnsureCached(std::string("artifacts/") + id.ToString() + "/" + name, fetch);
    if (!handle.ok()) {
        std::ostringstream os;
        os << "vm state artifact for snapshot '" << id.ToString() << "': " << handle.error();
        return core::make_unexpected(RepositoryError::ArtifactNotFound(os.str()));
    }
    const std::string path = handle.value()->path();
    // Pinned for as long as the caller holds the lease: the cache may
    // otherwise reclaim the memory dump while the VM that needs it is still
    // being built.
    lease->AddHandle(handle.value());
    return path;
}

RepositoryResult<sandbox::SandboxSnapshotManifest>
RepositoryRuntimeResolver::LoadFirecrackerManifest(const core::SnapshotId& id) {
    const BlobKey key = BlobKey::Snapshot(id, SnapshotFile::FirecrackerManifest);
    const std::string name = SnapshotFileName(SnapshotFile::FirecrackerManifest);

    const RepositoryResult<BlobLocation> location = repository_->LocateBlob(key);
    if (!location.ok()) return core::make_unexpected(location.error());
    if (location.value().kind == BlobLocation::Kind::Local) {
        // No existence pre-check, matching upstream: the loader reports a
        // missing or malformed manifest with the path in the message, so a
        // check here would only duplicate it.
        return LoadFirecrackerManifestFromPath(location.value().value);
    }

    // Fetched into memory rather than onto disk -- a manifest is a small JSON
    // document, and `ReadBlob` avoids a cache entry per resolve for something
    // parsed once and discarded.
    if (p2p_transport_) {
        const p2p::P2pArtifactKey p2p_key = FixedArtifactKey(id, name);
        const core::Expected<std::vector<uint8_t>, std::string> bytes =
            FetchArtifactBytes(p2p_transport_, p2p_key);
        if (bytes.ok()) {
            const std::string text(bytes.value().begin(), bytes.value().end());
            const RepositoryResult<sandbox::SandboxSnapshotManifest> parsed =
                ParseFirecrackerManifest(text, p2p_key);
            if (parsed.ok()) return parsed.value();
            // A peer served *something* that does not parse. Upstream falls
            // back rather than failing, and the distinction from the fetch
            // failure below is kept because it means something different: a
            // reachable peer with a corrupt or stale artifact.
            AGENTENV_DEBUG("P2P firecracker manifest parse failed; using repository fallback: "
                           << parsed.error().ToString());
        } else {
            AGENTENV_DEBUG("P2P firecracker manifest fetch failed; using repository fallback: "
                           << bytes.error());
        }
    }

    const RepositoryResult<std::string> bytes = repository_->ReadBlob(key);
    if (!bytes.ok()) return core::make_unexpected(bytes.error());
    std::ostringstream ref;
    ref << name << " of snapshot '" << id.ToString() << "'";
    return ParseFirecrackerManifest(bytes.value(), ref.str());
}

// ---- materialisation --------------------------------------------------------

RepositoryResult<std::string>
RepositoryRuntimeResolver::MaterializeImageConfig(
    const std::vector<OverlaybdLayerRef>& layers, const std::string& destination,
    const MaterializeSpec& spec) {
    const RepositoryResult<core::Unit> validated =
        ValidateManagedLayers(*repository_, layers, spec.label, spec.role);
    if (!validated.ok()) return core::make_unexpected(validated.error());

    ResolveContext rc;
    rc.resolver = this;
    rc.role     = spec.role;

    // The repository's URL for managed layers, if it has one. Absent on a
    // shared filesystem, where the layers are read by path instead.
    const core::Optional<std::string> managed_repo_blob_url =
        repository_->ManagedLayersRepoBlobUrl();

    return image_materializer_.MaterializeImageConfig(
        layers, destination, spec.label,
        managed_repo_blob_url.has_value() ? *managed_repo_blob_url : std::string(),
        &ResolveManagedCallback, &rc);
}

RepositoryResult<std::string>
RepositoryRuntimeResolver::MaterializeAndPin(const std::vector<OverlaybdLayerRef>& layers,
                                             const std::string& destination,
                                             const MaterializeSpec& spec,
                                             CacheArtifactLease* lease) {
    if (!spec.allow_empty_layers && layers.empty()) {
        return core::make_unexpected(
            RepositoryError::InvalidRequest(spec.label + " has no layers"));
    }

    // The cache's fetch contract is `Expected<uint64_t, std::string>`, which
    // would flatten a `RepositoryError` into text and lose its kind. Rust's
    // fetch returns `anyhow::Error`, so `materialize_image_config_error` can
    // downcast an `ArtifactNotFound` cause out of the chain and lift it. Carry
    // the typed cause out of the functor by reference to keep that classifier
    // working through the cache.
    core::Optional<RepositoryError> cause;

    // A functor rather than a lambda-with-captures so the cache can invoke
    // the fetch without re-entering this frame.
    struct Fetch {
        RepositoryRuntimeResolver*            resolver;
        const std::vector<OverlaybdLayerRef>* layers;
        const MaterializeSpec*                spec;
        core::Optional<RepositoryError>*      cause;

        core::Expected<uint64_t, std::string> operator()(const std::string& dest) const {
            const RepositoryResult<std::string> written =
                resolver->MaterializeImageConfig(*layers, dest, *spec);
            if (!written.ok()) {
                *cause = core::Optional<RepositoryError>(written.error());
                return core::make_unexpected(written.error().ToString());
            }
            const core::Expected<uint64_t, std::string> size =
                core::fs::FileSize(written.value());
            if (!size.ok()) return core::make_unexpected(size.error());
            return size.value();
        }
    };

    Fetch fetch;
    fetch.resolver = this;
    fetch.layers   = &layers;
    fetch.spec     = &spec;
    fetch.cause    = &cause;

    const core::Expected<CacheHandle*, std::string> handle =
        cache_->EnsureCachedAt(spec.cache_key, destination, fetch);
    if (!handle.ok()) {
        // A cause set by the fetch keeps its kind; anything else (cache I/O,
        // size mismatch) is a backend failure of this label.
        return core::make_unexpected(MaterializeImageConfigError(
            spec.label, cause.has_value()
                            ? *cause
                            : RepositoryError::Backend(spec.label, handle.error())));
    }
    const std::string path = handle.value()->path();
    lease->AddHandle(handle.value());
    return path;
}

RepositoryResult<std::vector<ResolvedAttachedDrive> >
RepositoryRuntimeResolver::ResolveAttachedDrives(const core::SnapshotId& id,
                                                 const CommittedSnapshot& snapshot,
                                                 CacheArtifactLease* lease) {
    std::vector<ResolvedAttachedDrive> drives;
    for (std::size_t i = 0; i < snapshot.attached_drives.size(); ++i) {
        const CommittedAttachedDrive& drive = snapshot.attached_drives[i];

        if (drive.virtual_size == 0) {
            return core::make_unexpected(RepositoryError::InvalidRequest(
                std::string("attached drive '") + drive.drive_id +
                "' virtual_size must be non-zero"));
        }

        MaterializeSpec spec;
        spec.label = std::string("attached drive '") + drive.drive_id +
                     "' for snapshot '" + id.ToString() + "'";
        spec.cache_key =
            RuntimeImageCacheKey(id, std::string("drives/") + drive.drive_id + "/image.json");
        spec.role               = LayerRole::AttachedDrive;
        spec.allow_empty_layers = false;

        const RepositoryResult<std::string> image_config = MaterializeAndPin(
            drive.layers, image_materializer_.DriveImageConfigPath(id, drive.drive_id), spec,
            lease);
        if (!image_config.ok()) return core::make_unexpected(image_config.error());

        ResolvedAttachedDrive resolved;
        resolved.kind              = ResolvedAttachedDrive::Kind::Overlaybd;
        resolved.drive_id          = drive.drive_id;
        resolved.image_config_path = image_config.value();
        resolved.read_only         = drive.read_only;
        resolved.virtual_size      = drive.virtual_size;

        const core::Expected<std::string, std::string> mount_path =
            sandbox::NormalizeMountPathForDrive(drive.drive_id, drive.mount_path);
        resolved.mount_path = mount_path.ok()
                                  ? mount_path.value()
                                  : sandbox::ExtraDrive::DefaultMountPath(drive.drive_id);
        resolved.sub_path = drive.sub_path;

        drives.push_back(resolved);
    }
    return drives;
}

// ---- entry point ------------------------------------------------------------

RepositoryResult<RunnableSnapshot>
RepositoryRuntimeResolver::Resolve(const SnapshotRecord& snapshot) {
    if (!snapshot.committed.has_value()) {
        return core::make_unexpected(RepositoryError::InvalidRequest(
            std::string("snapshot '") + snapshot.id.ToString() + "' is not ready"));
    }
    const CommittedSnapshot& committed = *snapshot.committed;

    // The lease owns every handle taken below, so an early return releases the
    // pins already acquired rather than leaking them until the cache's own
    // bookkeeping notices.
    std::shared_ptr<CacheArtifactLease> lease(new CacheArtifactLease());

    const RepositoryResult<std::string> vm_state_path = VmStatePath(snapshot.id, lease.get());
    if (!vm_state_path.ok()) return core::make_unexpected(vm_state_path.error());

    const RepositoryResult<sandbox::SandboxSnapshotManifest> committed_manifest =
        LoadFirecrackerManifest(snapshot.id);
    if (!committed_manifest.ok()) return core::make_unexpected(committed_manifest.error());

    std::vector<OverlaybdLayerRef> memory_layers;
    for (std::size_t i = 0; i < committed.memory_layers.size(); ++i) {
        memory_layers.push_back(OverlaybdLayerRef::Managed(committed.memory_layers[i]));
    }
    MaterializeSpec memory_spec;
    memory_spec.label = std::string("memory for snapshot '") + snapshot.id.ToString() + "'";
    memory_spec.cache_key = RuntimeImageCacheKey(snapshot.id, "memory/image.json");
    memory_spec.role      = LayerRole::Memory;
    // A snapshot may carry no memory dump and still needs a stable runtime
    // config path, which is why `Memory` is the role upstream exempts.
    memory_spec.allow_empty_layers = true;

    const RepositoryResult<std::string> mem_image_config = MaterializeAndPin(
        memory_layers, image_materializer_.MemoryImageConfigPath(snapshot.id), memory_spec,
        lease.get());
    if (!mem_image_config.ok()) return core::make_unexpected(mem_image_config.error());

    MaterializeSpec rootfs_spec;
    rootfs_spec.label     = std::string("snapshot '") + snapshot.id.ToString() + "'";
    rootfs_spec.cache_key = RuntimeImageCacheKey(snapshot.id, "rootfs/image.json");
    rootfs_spec.role      = LayerRole::Rootfs;
    rootfs_spec.allow_empty_layers = false;

    const RepositoryResult<std::string> rootfs_image_config =
        MaterializeAndPin(committed.rootfs_layers,
                          image_materializer_.RootfsImageConfigPath(snapshot.id), rootfs_spec,
                          lease.get());
    if (!rootfs_image_config.ok()) return core::make_unexpected(rootfs_image_config.error());

    const RepositoryResult<std::vector<ResolvedAttachedDrive> > attached_drives =
        ResolveAttachedDrives(snapshot.id, committed, lease.get());
    if (!attached_drives.ok()) return core::make_unexpected(attached_drives.error());

    const RepositoryResult<sandbox::SandboxSnapshotManifest> runtime_manifest =
        HydrateRuntimeManifest(committed_manifest.value(), vm_state_path.value(),
                               mem_image_config.value(), rootfs_image_config.value(),
                               attached_drives.value());
    if (!runtime_manifest.ok()) return core::make_unexpected(runtime_manifest.error());

    // **Not ported**: upstream also sets
    // `runtime_manifest.memory_startup_pack = self.startup_pack(id, committed)`.
    // Two things are missing for it and only one is this file's. The type is:
    // upstream's `ResolvedStartupPack::source` is an enum of `OssUrl` and
    // `LocalPath`, while this port's carries a bare `url` string, so the
    // in-place variant cannot be expressed. The consumer is the other:
    // nothing in this port reads `memory_startup_pack` -- the ublk daemon's
    // prefetch registration is not wired to it -- so resolving one here would
    // add a second complete-with-no-callers path of exactly the kind this
    // port keeps finding. Recorded instead; see the README entry.

    RunnableSnapshot runnable;
    runnable.record   = snapshot;
    runnable.manifest = runtime_manifest.value();
    runnable.lease    = lease;
    return runnable;
}

}  // namespace snapshot
}  // namespace agentenv
