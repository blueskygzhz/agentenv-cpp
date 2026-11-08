// SPDX-License-Identifier: MIT
// Test doubles for the `templateID` -> runnable-snapshot hop that
// `ApiImpl::CreateSandbox` performs (Rust
// `SnapshotManager::load_runnable`).
//
// Two fakes rather than one because the hop has two halves and they fail
// independently: the repository lookup (a missing record is the caller's
// fault, 400) and the resolve (a missing artifact is the node's, 500). A
// single fake would have made those indistinguishable.
//
// Deliberately *not* a stub that returns a default-constructed
// `RunnableSnapshot`: the whole point of resolving is that the manifest paths
// are real and the virtualization mode matches the node, and both are checked
// downstream. `CommittedRecord` below therefore builds a record a launch will
// actually accept.
#ifndef AGENTENV_TESTS_SNAPSHOT_RESOLVER_FAKES_H_
#define AGENTENV_TESTS_SNAPSHOT_RESOLVER_FAKES_H_

#include <memory>
#include <string>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/backends.h"
#include "agentenv/snapshot/resolver.h"

namespace agentenv {
namespace testing {

/// A committed record a snapshot launch accepts.
///
/// The virtualization mode is left at `CommittedSnapshot`'s default, which is
/// also `AppConfig`'s: `Orchestrator::CreateSandbox` refuses a snapshot whose
/// mode differs from the node's, so a mismatch here would fail every test for
/// a reason that has nothing to do with what it is testing.
inline snapshot::SnapshotRecord CommittedRecord(const core::SnapshotId& id) {
    snapshot::SnapshotRecord record;
    record.id = id;
    record.resources.cpu_count     = 2;
    record.resources.memory_mib    = 512;
    record.resources.disk_size_mib = 1024;

    snapshot::CommittedSnapshot committed;
    committed.context = snapshot::CommandContext::New(
        std::map<std::string, std::string>(), "/work");
    committed.runtime_versions.envd_version        = "0.5.15";
    committed.runtime_versions.tools_drive_version = "1.2.3";
    record.committed = core::Optional<snapshot::CommittedSnapshot>(committed);
    return record;
}

/// The hydrated manifest a resolver would hand back for `CommittedRecord`.
inline sandbox::SandboxSnapshotManifest ResolvedManifest() {
    sandbox::SandboxSnapshotManifest manifest;
    manifest.backend                  = sandbox::kFirecrackerBackend;
    manifest.vm_state.path            = "/snap/vm_state.bin";
    manifest.memory.image_config_path = "/snap/mem-image.json";
    manifest.memory.virtual_size      = 512 * 1024 * 1024;
    manifest.rootfs.image_config_path = "/snap/rootfs-image.json";
    manifest.rootfs.virtual_size      = 1024 * 1024 * 1024;
    // Reserved slots, and not incidentally: `CreateSandbox` refuses more
    // launch-time drives than the capture left free, so a manifest with zero
    // slots cannot take a `volumeMounts` request at all. Four is enough for
    // every test here and still small enough that exceeding it is easy.
    manifest.volume_drive_slots       = 4;
    return manifest;
}

/// A repository that answers `Get` from one canned record and nothing else.
///
/// `InMemorySnapshotRepository` cannot stand in here: its `Create` rejects a
/// committed record on purpose (`create` only pre-registers a template), so
/// there is no way to seed it with the thing under test.
class CannedSnapshotRepository : public snapshot::repository::SnapshotRepository {
 public:
    /// The id or alias `Get` answers for. Any other key misses.
    std::string                              key;
    core::Optional<snapshot::SnapshotRecord>  record;
    /// When set, `Get` reports this instead of answering.
    core::Optional<snapshot::repository::RepositoryError> fail;
    int get_calls = 0;

    snapshot::repository::RepositoryResult<core::Optional<snapshot::SnapshotRecord> >
        Get(const std::string& id_or_alias) override {
        ++get_calls;
        if (fail.has_value()) return core::make_unexpected(*fail);
        if (id_or_alias != key) {
            return core::Optional<snapshot::SnapshotRecord>();
        }
        return record;
    }

    snapshot::repository::RepositoryResult<snapshot::SnapshotRecord>
        Create(const snapshot::SnapshotRecord& r) override {
        return r;
    }
    snapshot::repository::RepositoryResult<std::vector<snapshot::SnapshotRecord> >
        List(const snapshot::repository::SnapshotListFilter&) override {
        return std::vector<snapshot::SnapshotRecord>();
    }
    snapshot::repository::RepositoryResult<core::Unit>
        Delete(const std::string&) override { return core::Unit(); }
    snapshot::repository::RepositoryResult<core::Optional<core::SnapshotId> >
        ResolveAlias(const std::string&) override {
        return core::Optional<core::SnapshotId>();
    }
    snapshot::repository::RepositoryResult<snapshot::SnapshotRecord>
        TryStartBuild(const core::SnapshotId& id) override {
        return core::make_unexpected(
            snapshot::repository::RepositoryError::SnapshotNotFound(id.ToString()));
    }
    snapshot::repository::RepositoryResult<core::Unit>
        MarkBuildError(const core::SnapshotId&,
                       const snapshot::TemplateBuildErrorReason&) override {
        return core::Unit();
    }
};

/// A resolver that hands back one canned `RunnableSnapshot`.
///
/// It copies the record it was *given* into the result rather than a stored
/// one, so a test cannot pass by accident when the handler resolves the wrong
/// id: the id that comes back is the id that went in.
class CannedRuntimeResolver : public snapshot::SnapshotRuntimeResolver {
 public:
    sandbox::SandboxSnapshotManifest manifest = ResolvedManifest();
    core::Optional<snapshot::repository::RepositoryError> fail;
    int  resolve_calls = 0;
    std::string last_resolved_id;

    snapshot::repository::RepositoryResult<snapshot::RunnableSnapshot>
        Resolve(const snapshot::SnapshotRecord& record) override {
        ++resolve_calls;
        last_resolved_id = record.id.ToString();
        if (fail.has_value()) return core::make_unexpected(*fail);
        snapshot::RunnableSnapshot runnable;
        runnable.record   = record;
        runnable.manifest = manifest;
        return runnable;
    }
};

/// Repository + resolver wired to one another over a fresh snapshot id.
struct SnapshotSourceFixture {
    core::SnapshotId                            id;
    std::shared_ptr<CannedSnapshotRepository>   repository;
    std::shared_ptr<CannedRuntimeResolver>      resolver;

    explicit SnapshotSourceFixture(const std::string& alias_or_id = std::string())
        : id(core::SnapshotId::Fresh()),
          repository(new CannedSnapshotRepository()),
          resolver(new CannedRuntimeResolver()) {
        repository->key = alias_or_id.empty() ? id.ToString() : alias_or_id;
        repository->record =
            core::Optional<snapshot::SnapshotRecord>(CommittedRecord(id));
    }

    /// The `templateID` a request must carry to hit this fixture.
    const std::string& template_id() const { return repository->key; }
};

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_SNAPSHOT_RESOLVER_FAKES_H_
