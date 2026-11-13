// SPDX-License-Identifier: MIT
// Rust: src/snapshot/publish.rs — publishing a captured snapshot: the
// repository stores its files and layers, then the catalog commits its record.
#ifndef AGENTENV_SNAPSHOT_PUBLISH_H_
#define AGENTENV_SNAPSHOT_PUBLISH_H_

#include <memory>
#include <vector>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/store.h"

namespace agentenv {
namespace p2p {
class P2pTransport;
}  // namespace p2p

namespace snapshot {

/// Rust `struct SnapshotPublication` — a committed snapshot.
struct SnapshotPublication {
    SnapshotRecord record;

    /// Rust `local_layers` — the upload files kept alive for the post-commit
    /// P2P announcement, straight from `StoredSnapshot`.
    ///
    /// Not part of the durable record: these are node-local paths, some of
    /// them temporaries. Holding them is what keeps a recontainerized or
    /// dense-exported layer's bytes on disk between the commit and
    /// `PublishSnapshotP2pArtifacts`.
    std::vector<PreparedLayerUpload> local_layers;
};

/// Rust `publish`.
///
/// The sequence, and what each step is for:
///
///   1. **Validate the attached drives** — a pure check, so a malformed
///      request costs no uploads.
///   2. **Refuse an already-committed snapshot.** Republishing would
///      overwrite the files the live record points at, and step 5's rollback
///      would then delete them. Checked here *and* again inside `commit`:
///      this one keeps the upload from starting, the one in `commit` is the
///      atomic guarantee. Dropping either is wrong — without this one a
///      doomed publish still rewrites live blobs, and without `commit`'s a
///      concurrent publisher slips through the gap.
///   3. **Refuse a taken alias**, for the same reason and with the same
///      caveat: `commit` claims it atomically, this is the early out.
///   4. **Store the files and layers.**
///   5. **Commit the record.** On failure the stored files are deleted —
///      shared content-addressed layers excepted — and the cleanup failure is
///      logged rather than returned, because the caller must see the commit
///      error.
///
/// Not ported: the startup-manifest continuation (`spawn_startup_manifest` /
/// `finish_startup_manifest`), which builds the memory startup pack on a
/// background task after this returns and then calls
/// `catalog::attach_startup_manifest`. The attach half *is* ported (see
/// `record_catalog.h`); what is missing is the pack builder and the task that
/// drives it, so `committed.memory_startup` stays absent here — which is
/// exactly what upstream leaves it as when it returns, too.
repository::RepositoryResult<SnapshotPublication> Publish(
    repository::SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest);

/// `Publish` with the source-registry exporter injected, for the same reason
/// `StoreSnapshot` has the overload: the exporter is the one step that talks to
/// a registry, and the publish sequence around it cannot otherwise be tested.
///
/// The policy still decides whether it is used at all.
repository::RepositoryResult<SnapshotPublication> Publish(
    repository::SnapshotRepository& repository, const SnapshotPublishMetadata& metadata,
    const sandbox::SandboxSnapshotManifest& manifest,
    const SourceRegistryExporter& exporter);

/// Rust `SnapshotManager::publish_p2p_artifacts` — best-effort announcement of
/// a committed snapshot's artifacts to the P2P network.
///
/// Runs **after** the commit, never before, and never affects the publish
/// result: every failure is logged and stepped over, because P2P is an
/// optional acceleration path and a snapshot that cannot be announced is still
/// a usable snapshot. A null `transport` returns immediately, as does a record
/// that is not committed.
///
/// There is no `SnapshotManager` in this port (see `api/impls.h`), so this is a
/// free function over the pieces upstream's manager holds as fields. What gets
/// announced:
///
///   1. `vm_state.bin`, by its per-snapshot key, read from the capture path.
///   2. The serialised manifest, as bytes — compact JSON, matching upstream's
///      `serde_json::to_vec`, and so **not** byte-identical to the pretty copy
///      `StoreSnapshot` writes into the repository. Upstream has the same
///      split; the key does not depend on the bytes.
///   3. `publication.local_layers`, content-addressed. These describe the
///      committed bytes, including compressed and dense-exported ones.
///   4. Every local overlaybd layer the runtime image configs reference, for
///      rootfs, memory and each attached drive, gated by the digests and
///      uuids the committed record actually names.
///
/// Deduplicated by key, because an unchanged local layer appears in both the
/// uploads and the image config.
void PublishSnapshotP2pArtifacts(const std::shared_ptr<p2p::P2pTransport>& transport,
                                 const SnapshotPublication& publication,
                                 const sandbox::SandboxSnapshotManifest& manifest);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_PUBLISH_H_
