// SPDX-License-Identifier: MIT
// Rust: src/volume.rs — `VolumeManager` and `VolumeLimits`.
//
// Porting notes
// -------------
// * Every Rust method is `async fn` over `tokio`. C++11 has no coroutines, so
//   the port is synchronous and blocking. The orchestrator calls these from its
//   own worker threads, which is the same concurrency shape the Rust runtime
//   provides for this module (no method here is CPU-bound or fans out).
// * `Arc<RwLock<HashMap<..>>>` becomes a `std::map` guarded by a
//   `std::mutex`. Rust distinguishes read and write locks; the critical
//   sections here are all tiny map lookups, so a plain mutex keeps the same
//   invariants without a reader-writer implementation.
// * `Arc<dyn SnapshotRepository>` becomes `std::shared_ptr<SnapshotRepository>`.
// * The two backing-creation paths shell out (`mkfs.ext4`,
//   `package_raw_as_overlaybd`). They are gated: without those tools the call
//   returns a precise `Storage` error instead of silently producing a bad
//   volume. See `CreateEmptyBacking`.
#ifndef AGENTENV_VOLUME_H_
#define AGENTENV_VOLUME_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/volume/record.h"

namespace agentenv {
namespace volume {

/// Rust struct `VolumeLimits`.
struct VolumeLimits {
    uint64_t max_size_mb = 0;
    std::size_t max_mounts = 0;

    /// Rust `impl Default for VolumeLimits`, which reads `cfg::VolumeConfig`
    /// defaults. Those are duplicated as literals here so this header does not
    /// depend on `cfg.h`; `VolumeLimitsFromConfig` is the wiring point when a
    /// real `AppConfig` is available.
    static VolumeLimits Default();
};

/// Rust `repository_error` — maps the repository's not-found / name-conflict
/// variants onto their volume equivalents and everything else onto `Storage`.
VolumeError RepositoryErrorToVolumeError(const snapshot::repository::RepositoryError& error);

/// Rust `current_local_backing` — the node-local image config is only usable
/// when it belongs to the same layer set as the durable record *and* still
/// exists on disk. A stale local file must never shadow committed layers.
core::Optional<std::string> CurrentLocalBacking(
    const VolumeRecord& remote, const std::map<std::string, VolumeRecord>& local);

/// Rust struct `VolumeManager`.
class VolumeManager {
 public:
    /// Rust `VolumeManager::open_with_repository`.
    static VolumeResult<std::shared_ptr<VolumeManager> > OpenWithRepository(
        const std::string& catalog_path,
        std::shared_ptr<snapshot::repository::SnapshotRepository> repository);

    /// Rust `VolumeManager::open_with_repository_and_limits`. `max_mounts` is
    /// clamped to `kMaxVolumeMounts`, as upstream does.
    static VolumeResult<std::shared_ptr<VolumeManager> > OpenWithRepositoryAndLimits(
        const std::string& catalog_path,
        std::shared_ptr<snapshot::repository::SnapshotRepository> repository,
        VolumeLimits limits);

    /// Rust `VolumeManager::limits`.
    VolumeLimits limits() const { return limits_; }

    /// Rust `VolumeManager::data_dir` — `{root}/data/{volume_id}`.
    std::string DataDir(const std::string& volume_id) const;

    /// Rust `VolumeManager::get`. Validates the catalog's id and overlays the
    /// node-local backing path.
    VolumeResult<VolumeRecord> Get(const std::string& reference);

    /// Rust `VolumeManager::list_page`.
    VolumeResult<VolumePage> ListPage(const core::Optional<std::string>& next_token,
                                      std::size_t limit);

    /// Rust `VolumeManager::materialize_backing`.
    VolumeResult<VolumeRecord> MaterializeBacking(const std::string& reference);

    /// Rust `VolumeManager::create`.
    VolumeResult<VolumeRecord> Create(const std::string& name, VolumeMode mode,
                                      const core::Optional<std::string>& from_volume,
                                      const core::Optional<std::string>& source_config,
                                      uint64_t size_mb);

    /// Rust `VolumeManager::create_build_cache` — a private writable child that
    /// is *not* published; the caller holds a read lease on the seed.
    VolumeResult<VolumeRecord> CreateBuildCache(const std::string& name,
                                                const core::Optional<std::string>& seed,
                                                uint64_t size_mb, const std::string& owner);

    /// Rust `VolumeManager::create_child_for_owner`.
    VolumeResult<VolumeRecord> CreateChildForOwner(const std::string& reference,
                                                   const std::string& name, VolumeMode mode,
                                                   uint64_t size_mb,
                                                   const std::string& source_owner,
                                                   const std::string& child_owner);

    /// Rust `VolumeManager::create_from_snapshot`.
    VolumeResult<VolumeRecord> CreateFromSnapshot(
        const std::string& name, VolumeMode mode, uint64_t size_mb,
        const std::vector<snapshot::OverlaybdLayerRef>& backing_layers);

    /// Rust `VolumeManager::snapshot_volume_state`.
    VolumeResult<VolumeRecord> SnapshotVolumeState(const std::string& reference);

    /// Rust `VolumeManager::delete`. (`delete` is a C++ keyword.)
    core::Expected<core::Unit, VolumeError> Remove(const std::string& reference);

    /// Rust `VolumeManager::reserve`.
    core::Expected<core::Unit, VolumeError> Reserve(const std::string& reference,
                                                    const std::string& owner);

    /// Rust `VolumeManager::replace_owner_for`.
    core::Expected<core::Unit, VolumeError> ReplaceOwnerFor(
        const std::string& owner, const core::Optional<std::string>& new_owner,
        const std::vector<std::string>& volume_ids);

    /// Rust `VolumeManager::fail_backings` — keeps an incomplete capture
    /// unavailable after its sandbox stops.
    core::Expected<core::Unit, VolumeError> FailBackings(
        const std::string& owner, const std::vector<std::string>& volume_ids);

    /// Rust `VolumeManager::publish_backings`.
    core::Expected<core::Unit, VolumeError> PublishBackings(
        const std::string& owner, const std::vector<std::string>& volume_ids);

    /// Rust `VolumeManager::recover_backings` — restores deterministic
    /// node-local config paths after a server restart.
    core::Expected<core::Unit, VolumeError> RecoverBackings(
        const std::string& owner, const std::vector<std::string>& volume_ids);

    /// Rust `VolumeManager::recover_and_publish_backings`.
    core::Expected<core::Unit, VolumeError> RecoverAndPublishBackings(
        const std::string& owner, const std::vector<std::string>& volume_ids);

    /// Rust `VolumeManager::persist_catalog`. Exposed because the upstream
    /// tests drive status transitions through it directly.
    core::Expected<core::Unit, VolumeError> PersistCatalog(const VolumeRecord& record);

 private:
    VolumeManager() {}

    /// Rust `VolumeManager::create_with_source_owner` — the single code path
    /// behind `create`, `create_build_cache` and `create_child_for_owner`.
    VolumeResult<VolumeRecord> CreateWithSourceOwner(
        const std::string& name, VolumeMode mode,
        const core::Optional<std::string>& from_volume,
        const core::Optional<std::string>& from_volume_source_owner,
        const core::Optional<std::string>& source_config, uint64_t size_mb,
        const core::Optional<std::string>& reserved_owner);

    /// Rust `VolumeManager::publish_records`.
    core::Expected<core::Unit, VolumeError> PublishRecords(const std::string& owner,
                                                           std::vector<VolumeRecord> records);
    /// Rust `VolumeManager::mark_publication_failed`.
    void MarkPublicationFailed(VolumeRecord record, const char* stage, const VolumeError& error);
    /// Rust `VolumeManager::cache_record`.
    void CacheRecord(const VolumeRecord& record);
    /// Rust `VolumeManager::publish_backing`.
    core::Expected<core::Unit, VolumeError> PublishBacking(VolumeRecord* record);
    /// Rust `VolumeManager::create_empty_backing` — needs `mkfs.ext4` and the
    /// overlaybd packer; returns a `Storage` error when they are unavailable.
    VolumeResult<std::string> CreateEmptyBacking(const std::string& volume_id, uint64_t size_mb);
    /// Rust `VolumeManager::create_child_backing` — hard-links (falling back to
    /// copy) every local lower into the child's directory.
    VolumeResult<std::string> CreateChildBacking(const std::string& volume_id,
                                                 const std::string& parent_config);

    std::map<std::string, VolumeRecord> records_;
    mutable std::mutex records_mutex_;
    std::string root_;
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository_;
    VolumeLimits limits_;
};

}  // namespace volume
}  // namespace agentenv
#endif  // AGENTENV_VOLUME_H_
