// SPDX-License-Identifier: MIT
// Rust: src/snapshot/publish.rs — publishing a captured snapshot: the
// repository stores its files and layers, then the catalog commits its record.
#ifndef AGENTENV_SNAPSHOT_PUBLISH_H_
#define AGENTENV_SNAPSHOT_PUBLISH_H_

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/store.h"

namespace agentenv {
namespace snapshot {

/// Rust `struct SnapshotPublication` — a committed snapshot.
///
/// Rust also carries `local_layers`, the upload files kept alive for
/// post-commit P2P announcement. That transport is not ported, and an empty
/// field here would imply it could be populated. Temporary paths are never
/// stored in the durable record either way.
struct SnapshotPublication {
    SnapshotRecord record;
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

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_PUBLISH_H_
