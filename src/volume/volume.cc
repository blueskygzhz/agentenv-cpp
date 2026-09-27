// SPDX-License-Identifier: MIT
// Rust: src/volume.rs — `VolumeManager`.
#include "agentenv/volume.h"

#include <cstdlib>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace volume {
namespace {

using core::Optional;
using core::Unit;
using snapshot::OverlaybdLayerRef;
namespace obd = storage::overlaybd;
namespace repo = snapshot::repository;

/// Rust `format!("vol_{}", Uuid::now_v7().simple())`. `simple()` is the
/// dash-free hex form, so the dashes from `Uuid::ToString` are stripped here —
/// which also keeps the id inside `is_valid_volume_component`.
std::string FreshVolumeId() {
    const std::string dashed = core::Uuid::GenV7().ToString();
    std::string simple;
    simple.reserve(32);
    for (std::size_t i = 0; i < dashed.size(); ++i) {
        if (dashed[i] != '-') simple.push_back(dashed[i]);
    }
    return "vol_" + simple;
}

/// Runs `argv` and returns its exit status, or an error when it could not be
/// spawned. Replaces `tokio::process::Command::status()`.
core::Expected<int, std::string> RunProgram(const std::vector<std::string>& argv) {
    if (argv.empty()) return core::make_unexpected(std::string("empty command"));

    const pid_t pid = ::fork();
    if (pid < 0) return core::make_unexpected(std::string("fork failed"));
    if (pid == 0) {
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (std::size_t i = 0; i < argv.size(); ++i) {
            raw.push_back(const_cast<char*>(argv[i].c_str()));
        }
        raw.push_back(NULL);
        ::execvp(raw[0], &raw[0]);
        ::_exit(127);  // exec failed
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        // retry on EINTR
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/// Renders a process status the way Rust's `ExitStatus` Display does, so the
/// `Storage` message text matches upstream closely enough to be recognisable.
std::string DescribeStatus(int code) {
    std::ostringstream oss;
    oss << "exit status: " << code;
    return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

VolumeLimits VolumeLimits::Default() {
    // Rust reads `cfg::VolumeConfig::default()`: `max_size_mb` defaults to
    // `DEFAULT_VOLUME_SIZE_MB` and `max_volume_count` to `MAX_VOLUME_MOUNTS`.
    VolumeLimits limits;
    limits.max_size_mb = kDefaultVolumeSizeMb;
    limits.max_mounts = kMaxVolumeMounts;
    return limits;
}

VolumeError RepositoryErrorToVolumeError(const repo::RepositoryError& error) {
    // Rust `repository_error`. Only the two volume-shaped variants are
    // translated; anything else keeps its message under `Storage`.
    switch (error.kind) {
        case repo::RepositoryErrorKind::SnapshotNotFound:
            return VolumeError::NotFound(error.message);
        case repo::RepositoryErrorKind::AliasConflict:
            return VolumeError::NameConflict(error.alias);
        default:
            return VolumeError::Storage(error.ToString());
    }
}

core::Optional<std::string> CurrentLocalBacking(
    const VolumeRecord& remote, const std::map<std::string, VolumeRecord>& local) {
    const std::map<std::string, VolumeRecord>::const_iterator found = local.find(remote.id);
    if (found == local.end()) return Optional<std::string>();
    // Same layer set, or the local file describes a different generation.
    if (!snapshot::LayerRefsEqual(found->second.backing_layers, remote.backing_layers)) {
        return Optional<std::string>();
    }
    if (!found->second.backing_image_config.has_value()) return Optional<std::string>();
    const std::string& path = *found->second.backing_image_config;
    if (!core::fs::Exists(path)) return Optional<std::string>();
    return Optional<std::string>(path);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

VolumeResult<std::shared_ptr<VolumeManager> > VolumeManager::OpenWithRepository(
    const std::string& catalog_path, std::shared_ptr<repo::SnapshotRepository> repository) {
    return OpenWithRepositoryAndLimits(catalog_path, repository, VolumeLimits::Default());
}

VolumeResult<std::shared_ptr<VolumeManager> > VolumeManager::OpenWithRepositoryAndLimits(
    const std::string& catalog_path, std::shared_ptr<repo::SnapshotRepository> repository,
    VolumeLimits limits) {
    const Optional<std::string> parent = core::fs::Parent(catalog_path);
    if (!parent.has_value()) {
        // Rust: `anyhow!("volume catalog path has no parent")`.
        return core::make_unexpected(
            VolumeError::Storage("volume catalog path has no parent"));
    }
    if (!repository) {
        return core::make_unexpected(VolumeError::Storage("volume repository is null"));
    }

    limits.max_mounts = limits.max_mounts < kMaxVolumeMounts ? limits.max_mounts : kMaxVolumeMounts;

    std::shared_ptr<VolumeManager> manager(new VolumeManager());
    manager->root_ = *parent;
    manager->repository_ = repository;
    manager->limits_ = limits;
    return manager;
}

std::string VolumeManager::DataDir(const std::string& volume_id) const {
    return core::fs::Join(core::fs::Join(root_, "data"), volume_id);
}

void VolumeManager::CacheRecord(const VolumeRecord& record) {
    std::lock_guard<std::mutex> guard(records_mutex_);
    records_[record.id] = record;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

VolumeResult<VolumeRecord> VolumeManager::Get(const std::string& reference) {
    repo::RepositoryResult<Optional<VolumeRecord> > fetched = repository_->GetVolume(reference);
    if (!fetched.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(fetched.error()));
    }
    if (!fetched.value().has_value()) {
        return core::make_unexpected(VolumeError::NotFound(reference));
    }

    VolumeRecord record = *fetched.value();
    const core::Expected<Unit, VolumeError> valid = ValidateVolumeId(record.id);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    {
        std::lock_guard<std::mutex> guard(records_mutex_);
        record.backing_image_config = CurrentLocalBacking(record, records_);
    }
    return record;
}

VolumeResult<VolumePage> VolumeManager::ListPage(const Optional<std::string>& next_token,
                                                 std::size_t limit) {
    if (limit == 0) return core::make_unexpected(VolumeError::InvalidPageLimit());

    Optional<std::string> after_volume_id;
    if (next_token.has_value()) {
        const VolumeResult<std::string> decoded = DecodeVolumeCursor(*next_token);
        if (!decoded.ok()) return core::make_unexpected(decoded.error());
        after_volume_id = decoded.value();
    }

    repo::RepositoryResult<repo::VolumeRecordPage> fetched =
        repository_->ListVolumesPage(after_volume_id, limit);
    if (!fetched.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(fetched.error()));
    }

    VolumePage page;
    page.records = fetched.value().records;
    {
        std::lock_guard<std::mutex> guard(records_mutex_);
        for (std::size_t i = 0; i < page.records.size(); ++i) {
            const core::Expected<Unit, VolumeError> valid = ValidateVolumeId(page.records[i].id);
            if (!valid.ok()) return core::make_unexpected(valid.error());
            page.records[i].backing_image_config = CurrentLocalBacking(page.records[i], records_);
        }
    }
    if (fetched.value().next_volume_id.has_value()) {
        page.next_token = EncodeVolumeCursor(*fetched.value().next_volume_id);
    }
    return page;
}

VolumeResult<VolumeRecord> VolumeManager::MaterializeBacking(const std::string& reference) {
    VolumeResult<VolumeRecord> loaded = Get(reference);
    if (!loaded.ok()) return loaded;
    VolumeRecord record = loaded.value();

    const bool local_is_usable = record.backing_image_config.has_value() &&
                                 core::fs::Exists(*record.backing_image_config);
    if (local_is_usable || record.backing_layers.empty()) return record;

    const std::string destination = core::fs::Join(DataDir(record.id), "image.json");
    AGENTENV_DEBUG("materializing volume backing volume_id=" << record.id << " layer_count="
                                                             << record.backing_layers.size());

    repo::RepositoryResult<std::string> materialized =
        repository_->MaterializeVolumeBacking(record.id, record.backing_layers, destination);
    if (!materialized.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(materialized.error()));
    }

    AGENTENV_DEBUG("volume backing materialized volume_id=" << record.id
                                                            << " path=" << materialized.value());
    record.backing_image_config = materialized.value();
    CacheRecord(record);
    return record;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

VolumeResult<VolumeRecord> VolumeManager::Create(const std::string& name, VolumeMode mode,
                                                 const Optional<std::string>& from_volume,
                                                 const Optional<std::string>& source_config,
                                                 uint64_t size_mb) {
    return CreateWithSourceOwner(name, mode, from_volume, Optional<std::string>(), source_config,
                                 size_mb, Optional<std::string>());
}

VolumeResult<VolumeRecord> VolumeManager::CreateBuildCache(const std::string& name,
                                                           const Optional<std::string>& seed,
                                                           uint64_t size_mb,
                                                           const std::string& owner) {
    return CreateWithSourceOwner(name, VolumeMode::Exclusive, seed, Optional<std::string>(),
                                 Optional<std::string>(), size_mb, Optional<std::string>(owner));
}

VolumeResult<VolumeRecord> VolumeManager::CreateChildForOwner(const std::string& reference,
                                                              const std::string& name,
                                                              VolumeMode mode, uint64_t size_mb,
                                                              const std::string& source_owner,
                                                              const std::string& child_owner) {
    return CreateWithSourceOwner(name, mode, Optional<std::string>(reference),
                                 Optional<std::string>(source_owner), Optional<std::string>(),
                                 size_mb, Optional<std::string>(child_owner));
}

VolumeResult<VolumeRecord> VolumeManager::CreateWithSourceOwner(
    const std::string& name, VolumeMode mode, const Optional<std::string>& from_volume,
    const Optional<std::string>& from_volume_source_owner,
    const Optional<std::string>& source_config, uint64_t size_mb,
    const Optional<std::string>& reserved_owner) {
    const core::Expected<Unit, VolumeError> named = ValidateName(name);
    if (!named.ok()) return core::make_unexpected(named.error());
    if (size_mb == 0) return core::make_unexpected(VolumeError::InvalidSize());
    if (size_mb > limits_.max_size_mb) {
        return core::make_unexpected(VolumeError::SizeLimitExceeded(limits_.max_size_mb));
    }
    if (from_volume.has_value() && source_config.has_value()) {
        return core::make_unexpected(VolumeError::MultipleSources());
    }

    // A name collision is detected by a successful lookup; only `NotFound`
    // clears the way.
    {
        const VolumeResult<VolumeRecord> existing = Get(name);
        if (existing.ok()) return core::make_unexpected(VolumeError::NameConflict(name));
        if (existing.error().kind != VolumeErrorKind::NotFound) {
            return core::make_unexpected(existing.error());
        }
    }

    const std::string id = FreshVolumeId();
    AGENTENV_DEBUG("creating volume volume_id=" << id << " mode=" << VolumeModeToString(mode)
                                                << " size_mb=" << size_mb);

    std::vector<OverlaybdLayerRef> backing_layers;
    Optional<std::string> backing_image_config;

    if (from_volume.has_value()) {
        VolumeResult<VolumeRecord> fetched = Get(*from_volume);
        if (!fetched.ok()) {
            if (fetched.error().kind == VolumeErrorKind::NotFound) {
                return core::make_unexpected(VolumeError::SourceNotFound(*from_volume));
            }
            return core::make_unexpected(fetched.error());
        }
        VolumeRecord parent = fetched.value();

        if (parent.status == VolumeStatus::Uploading) {
            return core::make_unexpected(VolumeError::Uploading(parent.id));
        }
        if (parent.status == VolumeStatus::Failed) {
            return core::make_unexpected(VolumeError::Failed(parent.id));
        }
        if (parent.reserved_by_sandbox_id.has_value()) {
            const std::string& owner = *parent.reserved_by_sandbox_id;
            const bool owner_matches = from_volume_source_owner.has_value() &&
                                       *from_volume_source_owner == owner;
            if (!owner_matches) return core::make_unexpected(VolumeError::Reserved(owner));
        }
        if (parent.size_mb != size_mb) {
            return core::make_unexpected(VolumeError::SizeMismatch());
        }

        if (!parent.reserved_by_sandbox_id.has_value() && !parent.backing_layers.empty()) {
            // Committed layers are immutable and repository-owned. Materializing
            // the child on mount preserves their descriptors without copying or
            // republishing the parent's local files. Reserved parents may have
            // newer local writes, so they still need the local clone below.
            backing_layers = parent.backing_layers;
        } else {
            if (!parent.backing_image_config.has_value() && !parent.backing_layers.empty()) {
                VolumeResult<VolumeRecord> refreshed = MaterializeBacking(parent.id);
                if (!refreshed.ok()) return core::make_unexpected(refreshed.error());
                parent = refreshed.value();
            }
            if (parent.backing_image_config.has_value()) {
                VolumeResult<std::string> cloned =
                    CreateChildBacking(id, *parent.backing_image_config);
                if (!cloned.ok()) return core::make_unexpected(cloned.error());
                backing_image_config = cloned.value();
            }
        }
    } else if (source_config.has_value()) {
        VolumeResult<std::string> cloned = CreateChildBacking(id, *source_config);
        if (!cloned.ok()) return core::make_unexpected(cloned.error());
        backing_image_config = cloned.value();
    } else {
        VolumeResult<std::string> created = CreateEmptyBacking(id, size_mb);
        if (!created.ok()) return core::make_unexpected(created.error());
        backing_image_config = created.value();
    }

    VolumeRecord record;
    record.id = id;
    record.name = name;
    record.mode = mode;
    record.size_mb = size_mb;
    record.status = VolumeStatus::Ready;
    record.reserved_by_sandbox_id = reserved_owner;
    record.backing_image_config = backing_image_config;
    record.backing_layers = backing_layers;
    record.deleting = false;

    // Rust wraps publish + insert in an async block and unwinds the on-disk
    // directory if any step fails.
    Optional<VolumeError> failure;
    if (!reserved_owner.has_value()) {
        const core::Expected<Unit, VolumeError> published = PublishBacking(&record);
        if (!published.ok()) failure = published.error();
    }
    if (!failure.has_value()) {
        repo::RepositoryResult<Unit> created = repository_->CreateVolume(record);
        if (!created.ok()) failure = RepositoryErrorToVolumeError(created.error());
    }
    if (failure.has_value()) {
        if (record.backing_image_config.has_value()) {
            const Optional<std::string> directory =
                core::fs::Parent(*record.backing_image_config);
            if (directory.has_value()) core::fs::RemoveDirAll(*directory);
        }
        return core::make_unexpected(*failure);
    }

    CacheRecord(record);
    AGENTENV_INFO("volume created volume_id=" << record.id
                                             << " mode=" << VolumeModeToString(record.mode)
                                             << " size_mb=" << record.size_mb);
    return record;
}

VolumeResult<VolumeRecord> VolumeManager::CreateFromSnapshot(
    const std::string& name, VolumeMode mode, uint64_t size_mb,
    const std::vector<OverlaybdLayerRef>& backing_layers) {
    const core::Expected<Unit, VolumeError> named = ValidateName(name);
    if (!named.ok()) return core::make_unexpected(named.error());
    if (size_mb == 0 || backing_layers.empty()) {
        return core::make_unexpected(VolumeError::InvalidSize());
    }
    if (size_mb > limits_.max_size_mb) {
        return core::make_unexpected(VolumeError::SizeLimitExceeded(limits_.max_size_mb));
    }
    {
        const VolumeResult<VolumeRecord> existing = Get(name);
        if (existing.ok()) return core::make_unexpected(VolumeError::NameConflict(name));
        if (existing.error().kind != VolumeErrorKind::NotFound) {
            return core::make_unexpected(existing.error());
        }
    }

    VolumeRecord record;
    record.id = FreshVolumeId();
    record.name = name;
    record.mode = mode;
    record.size_mb = size_mb;
    record.status = VolumeStatus::Ready;
    record.backing_layers = backing_layers;
    record.deleting = false;

    AGENTENV_DEBUG("creating volume from snapshot volume_id="
                   << record.id << " mode=" << VolumeModeToString(record.mode)
                   << " size_mb=" << record.size_mb);

    repo::RepositoryResult<Unit> created = repository_->CreateVolume(record);
    if (!created.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(created.error()));
    }
    CacheRecord(record);
    AGENTENV_INFO("volume created from snapshot volume_id="
                  << record.id << " mode=" << VolumeModeToString(record.mode)
                  << " size_mb=" << record.size_mb);
    return record;
}

VolumeResult<VolumeRecord> VolumeManager::SnapshotVolumeState(const std::string& reference) {
    VolumeResult<VolumeRecord> loaded = Get(reference);
    if (!loaded.ok()) return loaded;
    VolumeRecord parent = loaded.value();

    // A running sandbox restacks its volume upper into this same local image
    // config before capture. Publish that latest state before making the
    // logical volume snapshot.
    if (parent.reserved_by_sandbox_id.has_value()) {
        const std::string owner = *parent.reserved_by_sandbox_id;
        std::vector<std::string> ids;
        ids.push_back(parent.id);
        const core::Expected<Unit, VolumeError> republished =
            RecoverAndPublishBackings(owner, ids);
        if (!republished.ok()) return core::make_unexpected(republished.error());
        return Get(reference);
    }

    const core::Expected<Unit, VolumeError> published = PublishBacking(&parent);
    if (!published.ok()) return core::make_unexpected(published.error());
    const core::Expected<Unit, VolumeError> persisted = PersistCatalog(parent);
    if (!persisted.ok()) return core::make_unexpected(persisted.error());
    CacheRecord(parent);
    return parent;
}

// ---------------------------------------------------------------------------
// Deletion and reservations
// ---------------------------------------------------------------------------

core::Expected<Unit, VolumeError> VolumeManager::Remove(const std::string& reference) {
    const VolumeResult<VolumeRecord> loaded = Get(reference);
    if (!loaded.ok()) return core::make_unexpected(loaded.error());
    const VolumeRecord& record = loaded.value();

    if (record.reserved_by_sandbox_id.has_value()) {
        return core::make_unexpected(VolumeError::Reserved(*record.reserved_by_sandbox_id));
    }
    if (!record.read_only_mounts.empty()) {
        return core::make_unexpected(VolumeError::Reserved(record.read_only_mounts[0]));
    }

    AGENTENV_DEBUG("deleting volume volume_id=" << record.id);
    repo::RepositoryResult<Unit> deleted = repository_->DeleteVolume(record.id);
    if (!deleted.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(deleted.error()));
    }

    const std::string backing_directory = DataDir(record.id);
    {
        std::lock_guard<std::mutex> guard(records_mutex_);
        records_.erase(record.id);
    }
    // Rust downgrades a failed cleanup to a warning: the durable record is
    // already gone, so the node-local leftovers must not fail the call.
    const core::Expected<Unit, std::string> cleaned = core::fs::RemoveDirAll(backing_directory);
    if (!cleaned.ok()) {
        AGENTENV_WARN("failed to clean deleted volume's node-local backing volume_id="
                      << record.id << " path=" << backing_directory
                      << " error=" << cleaned.error());
    }
    AGENTENV_INFO("volume deleted volume_id=" << record.id);
    return Unit();
}

core::Expected<Unit, VolumeError> VolumeManager::Reserve(const std::string& reference,
                                                          const std::string& owner) {
    VolumeResult<VolumeRecord> loaded = Get(reference);
    if (!loaded.ok()) return core::make_unexpected(loaded.error());
    VolumeRecord record = loaded.value();

    if (record.status == VolumeStatus::Uploading) {
        return core::make_unexpected(VolumeError::Uploading(record.id));
    }
    if (record.status == VolumeStatus::Failed) {
        return core::make_unexpected(VolumeError::Failed(record.id));
    }

    if (record.mode == VolumeMode::ReadOnly) {
        repo::RepositoryResult<Unit> reserved =
            repository_->ReserveReadOnlyVolume(record.id, owner);
        if (!reserved.ok()) {
            return core::make_unexpected(RepositoryErrorToVolumeError(reserved.error()));
        }
        bool already_present = false;
        for (std::size_t i = 0; i < record.read_only_mounts.size(); ++i) {
            if (record.read_only_mounts[i] == owner) {
                already_present = true;
                break;
            }
        }
        if (!already_present) record.read_only_mounts.push_back(owner);
        AGENTENV_DEBUG("read-only volume reserved volume_id=" << record.id);
        CacheRecord(record);
        return Unit();
    }

    repo::RepositoryResult<Optional<std::string> > reserved =
        repository_->ReserveVolume(record.id, owner);
    if (!reserved.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(reserved.error()));
    }
    if (reserved.value().has_value()) {
        return core::make_unexpected(VolumeError::Reserved(*reserved.value()));
    }
    record.reserved_by_sandbox_id = owner;
    AGENTENV_DEBUG("exclusive volume reserved volume_id=" << record.id);
    CacheRecord(record);
    return Unit();
}

core::Expected<Unit, VolumeError> VolumeManager::ReplaceOwnerFor(
    const std::string& owner, const Optional<std::string>& new_owner,
    const std::vector<std::string>& volume_ids) {
    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        const std::string& volume_id = volume_ids[i];
        repo::RepositoryResult<Unit> replaced =
            repository_->ReplaceVolumeOwnerFor(volume_id, owner, new_owner);
        if (!replaced.ok()) {
            return core::make_unexpected(RepositoryErrorToVolumeError(replaced.error()));
        }
        {
            std::lock_guard<std::mutex> guard(records_mutex_);
            const std::map<std::string, VolumeRecord>::iterator found = records_.find(volume_id);
            if (found != records_.end()) found->second.ReplaceOwner(owner, new_owner);
        }
        if (new_owner.has_value()) {
            AGENTENV_DEBUG("volume reservation owner replaced volume_id="
                           << volume_id << " previous_owner=" << owner
                           << " new_owner=" << *new_owner);
        } else {
            AGENTENV_DEBUG("volume reservation released volume_id=" << volume_id
                                                                    << " owner=" << owner);
        }
    }
    return Unit();
}

// ---------------------------------------------------------------------------
// Publication lifecycle
// ---------------------------------------------------------------------------

core::Expected<Unit, VolumeError> VolumeManager::FailBackings(
    const std::string& owner, const std::vector<std::string>& volume_ids) {
    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        VolumeResult<VolumeRecord> loaded = Get(volume_ids[i]);
        if (!loaded.ok()) return core::make_unexpected(loaded.error());
        VolumeRecord record = loaded.value();
        if (!record.reserved_by_sandbox_id.has_value() ||
            *record.reserved_by_sandbox_id != owner) {
            continue;
        }
        record.status = VolumeStatus::Failed;
        const core::Expected<Unit, VolumeError> persisted = PersistCatalog(record);
        if (!persisted.ok()) return core::make_unexpected(persisted.error());
        CacheRecord(record);
    }
    return Unit();
}

core::Expected<Unit, VolumeError> VolumeManager::PublishBackings(
    const std::string& owner, const std::vector<std::string>& volume_ids) {
    std::vector<VolumeRecord> records;
    records.reserve(volume_ids.size());
    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        const VolumeResult<VolumeRecord> loaded = Get(volume_ids[i]);
        if (!loaded.ok()) return core::make_unexpected(loaded.error());
        if (loaded.value().MountedBy(owner)) records.push_back(loaded.value());
    }
    return PublishRecords(owner, records);
}

core::Expected<Unit, VolumeError> VolumeManager::RecoverBackings(
    const std::string& owner, const std::vector<std::string>& volume_ids) {
    for (std::size_t i = 0; i < volume_ids.size(); ++i) {
        VolumeResult<VolumeRecord> loaded = Get(volume_ids[i]);
        if (!loaded.ok()) return core::make_unexpected(loaded.error());
        VolumeRecord record = loaded.value();

        if (!record.reserved_by_sandbox_id.has_value() ||
            *record.reserved_by_sandbox_id != owner) {
            continue;
        }
        if (record.backing_image_config.has_value()) continue;

        const std::string path = core::fs::Join(DataDir(record.id), "image.json");
        if (!core::fs::Exists(path)) {
            // A reserved volume whose local backing vanished can never be
            // published again; mark it failed so no one mounts stale content.
            record.status = VolumeStatus::Failed;
            const core::Expected<Unit, VolumeError> persisted = PersistCatalog(record);
            if (!persisted.ok()) {
                AGENTENV_WARN("failed to persist volume failure status volume_id="
                              << record.id << " error=" << persisted.error().ToString());
            }
            CacheRecord(record);
            AGENTENV_WARN("reserved volume backing is missing volume_id=" << record.id
                                                                          << " path=" << path);
            std::ostringstream oss;
            oss << "local backing for reserved volume '" << record.id << "' is missing";
            return core::make_unexpected(VolumeError::Storage(oss.str()));
        }

        record.backing_image_config = path;
        AGENTENV_DEBUG("volume backing recovered volume_id=" << record.id << " owner=" << owner);
        CacheRecord(record);
    }
    return Unit();
}

core::Expected<Unit, VolumeError> VolumeManager::RecoverAndPublishBackings(
    const std::string& owner, const std::vector<std::string>& volume_ids) {
    const core::Expected<Unit, VolumeError> recovered = RecoverBackings(owner, volume_ids);
    if (!recovered.ok()) return recovered;
    return PublishBackings(owner, volume_ids);
}

core::Expected<Unit, VolumeError> VolumeManager::PublishRecords(
    const std::string& owner, std::vector<VolumeRecord> records) {
    for (std::size_t i = 0; i < records.size(); ++i) {
        VolumeRecord record = records[i];
        if (!record.reserved_by_sandbox_id.has_value() ||
            *record.reserved_by_sandbox_id != owner) {
            continue;
        }

        // Publish the state transition before uploading any writable upper
        // layer so other nodes cannot mount stale content.
        record.status = VolumeStatus::Uploading;
        core::Expected<Unit, VolumeError> persisted = PersistCatalog(record);
        if (!persisted.ok()) {
            const VolumeError error = persisted.error();
            MarkPublicationFailed(record, "mark_uploading", error);
            return core::make_unexpected(error);
        }
        CacheRecord(record);

        AGENTENV_DEBUG("publishing volume backing volume_id=" << record.id);
        const core::Expected<Unit, VolumeError> published = PublishBacking(&record);
        if (!published.ok()) {
            const VolumeError error = published.error();
            MarkPublicationFailed(record, "publish_backing", error);
            return core::make_unexpected(error);
        }

        record.status = VolumeStatus::Ready;
        persisted = PersistCatalog(record);
        if (!persisted.ok()) {
            const VolumeError error = persisted.error();
            MarkPublicationFailed(record, "mark_ready", error);
            return core::make_unexpected(error);
        }
        AGENTENV_INFO("volume backing published volume_id="
                      << record.id << " layer_count=" << record.backing_layers.size());
        CacheRecord(record);
    }
    return Unit();
}

void VolumeManager::MarkPublicationFailed(VolumeRecord record, const char* stage,
                                          const VolumeError& error) {
    record.status = VolumeStatus::Failed;
    const core::Expected<Unit, VolumeError> persisted = PersistCatalog(record);
    if (!persisted.ok()) {
        AGENTENV_WARN("failed to persist volume failure status volume_id="
                      << record.id << " error=" << persisted.error().ToString());
    }
    AGENTENV_WARN("volume backing publication failed volume_id=" << record.id << " stage=" << stage
                                                                 << " error=" << error.ToString());
    CacheRecord(record);
}

core::Expected<Unit, VolumeError> VolumeManager::PublishBacking(VolumeRecord* record) {
    if (!record->backing_image_config.has_value()) return Unit();
    repo::RepositoryResult<std::vector<OverlaybdLayerRef> > published =
        repository_->PublishVolumeBacking(record->id, *record->backing_image_config);
    if (!published.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(published.error()));
    }
    record->backing_layers = published.value();
    return Unit();
}

core::Expected<Unit, VolumeError> VolumeManager::PersistCatalog(const VolumeRecord& record) {
    repo::RepositoryResult<Unit> stored = repository_->PutVolume(record);
    if (!stored.ok()) {
        return core::make_unexpected(RepositoryErrorToVolumeError(stored.error()));
    }
    return Unit();
}

// ---------------------------------------------------------------------------
// Backing creation
// ---------------------------------------------------------------------------

VolumeResult<std::string> VolumeManager::CreateEmptyBacking(const std::string& volume_id,
                                                            uint64_t size_mb) {
    const std::string directory = DataDir(volume_id);
    const std::string raw = core::fs::Join(directory, "base.ext4");
    const std::string commit = core::fs::Join(directory, "base.commit");
    const std::string image_config = core::fs::Join(directory, "image.json");

    const core::Expected<Unit, std::string> created = core::fs::CreateDirAll(directory);
    if (!created.ok()) return core::make_unexpected(VolumeError::Storage(created.error()));

    // Rust wraps the body in an async block and removes the directory on any
    // failure; `failure` plays the role of that block's `Result`.
    Optional<VolumeError> failure;
    std::string result_path;

    do {
        if (size_mb > UINT64_MAX / kBytesPerMb) {
            failure = VolumeError::InvalidSize();  // Rust `checked_mul` overflow
            break;
        }
        const core::Expected<Unit, std::string> sized =
            core::fs::CreateSizedFile(raw, size_mb * kBytesPerMb);
        if (!sized.ok()) {
            failure = VolumeError::Storage(sized.error());
            break;
        }

        std::vector<std::string> mkfs;
        mkfs.push_back("mkfs.ext4");
        mkfs.push_back("-q");
        mkfs.push_back("-F");
        mkfs.push_back(raw);
        const core::Expected<int, std::string> status = RunProgram(mkfs);
        if (!status.ok()) {
            failure = VolumeError::Storage(status.error());
            break;
        }
        if (status.value() != 0) {
            std::ostringstream oss;
            oss << "mkfs.ext4 exited with status " << DescribeStatus(status.value());
            failure = VolumeError::Storage(oss.str());
            break;
        }

        // Rust: `overlaybd::tools::package_raw_as_overlaybd(&raw, &commit)`.
        // The C++ tree has no port of that packer yet, so rather than emit an
        // image.json pointing at a raw file that overlaybd cannot open, this
        // fails with an explicit message. Everything above (sizing, mkfs) is
        // already the real behaviour, so wiring the packer in is the only
        // remaining step.
        failure = VolumeError::Storage(
            "packaging a raw filesystem as an overlaybd commit layer is not implemented in "
            "the C++ port yet (Rust: overlaybd::tools::package_raw_as_overlaybd)");
        break;
    } while (false);

    if (failure.has_value()) {
        core::fs::RemoveDirAll(directory);
        return core::make_unexpected(*failure);
    }

    obd::ImageConfig config;
    obd::LayerConfig layer;
    layer.file = commit;
    config.lowers.push_back(layer);
    const core::Expected<bool, std::string> saved = obd::SaveImageConfig(image_config, config);
    if (!saved.ok()) {
        core::fs::RemoveDirAll(directory);
        return core::make_unexpected(VolumeError::Storage(saved.error()));
    }
    core::fs::RemoveFile(raw);
    result_path = image_config;
    return result_path;
}

VolumeResult<std::string> VolumeManager::CreateChildBacking(const std::string& volume_id,
                                                             const std::string& parent_config) {
    const std::string directory = DataDir(volume_id);
    const std::string image_config = core::fs::Join(directory, "image.json");

    const core::Expected<Unit, std::string> created = core::fs::CreateDirAll(directory);
    if (!created.ok()) return core::make_unexpected(VolumeError::Storage(created.error()));

    Optional<VolumeError> failure;
    do {
        const core::Expected<obd::ImageConfig, std::string> loaded =
            obd::LoadImageConfig(parent_config);
        if (!loaded.ok()) {
            failure = VolumeError::Storage(loaded.error());
            break;
        }
        obd::ImageConfig config = loaded.value();

        for (std::size_t index = 0; index < config.lowers.size(); ++index) {
            obd::LayerConfig& layer = config.lowers[index];
            // External layers carry no local file; they stay by reference.
            if (layer.file.empty()) continue;

            const Optional<std::string> extension = core::fs::Extension(layer.file);
            std::ostringstream name;
            name << "lower-" << index << "." << (extension.has_value() ? *extension : "layer");
            const std::string destination = core::fs::Join(directory, name.str());

            // Hard link first: a committed lower is immutable, so sharing the
            // inode is both correct and free. Copy is the cross-device fallback.
            const core::Expected<Unit, std::string> linked =
                core::fs::HardLink(layer.file, destination);
            if (!linked.ok()) {
                const core::Expected<Unit, std::string> copied =
                    core::fs::Copy(layer.file, destination);
                if (!copied.ok()) {
                    std::ostringstream oss;
                    oss << "clone volume layer '" << layer.file
                        << "' (hard link failed: " << linked.error()
                        << "; copy failed: " << copied.error() << ")";
                    failure = VolumeError::Storage(oss.str());
                    break;
                }
            }
            layer.file = destination;
        }
        if (failure.has_value()) break;

        const core::Expected<bool, std::string> saved =
            obd::SaveImageConfig(image_config, config);
        if (!saved.ok()) {
            failure = VolumeError::Storage(saved.error());
            break;
        }
    } while (false);

    if (failure.has_value()) {
        core::fs::RemoveDirAll(directory);
        return core::make_unexpected(*failure);
    }
    return image_config;
}

}  // namespace volume
}  // namespace agentenv
