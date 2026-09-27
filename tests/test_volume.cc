// SPDX-License-Identifier: MIT
// Rust: src/volume.rs `mod tests` plus coverage for the record/error helpers
// that the upstream tests exercise only indirectly.
#include "agentenv/volume.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
using agentenv::snapshot::ExternalLayer;
using agentenv::snapshot::ManagedLayer;
using agentenv::snapshot::OverlaybdLayerRef;
using agentenv::volume::VolumeError;
using agentenv::volume::VolumeErrorKind;
using agentenv::volume::VolumeManager;
using agentenv::volume::VolumeMode;
using agentenv::volume::VolumeRecord;
using agentenv::volume::VolumeStatus;
namespace obd = agentenv::storage::overlaybd;
namespace repo = agentenv::snapshot::repository;

/// Stands in for the Rust tests' `PosixFsBackend`: a durable catalog plus the
/// layer store, kept in memory but with the same ordering, aliasing and
/// reservation semantics the volume manager relies on.
class FakeVolumeRepository : public repo::SnapshotRepository {
 public:
    explicit FakeVolumeRepository(const std::string& store_root) : store_root_(store_root) {}

    // --- snapshot half: unused here, so minimally satisfied ----------------
    repo::RepositoryResult<repo::SnapshotRecord> Create(const repo::SnapshotRecord& record) {
        return record;
    }
    repo::RepositoryResult<Optional<repo::SnapshotRecord> > Get(const std::string&) {
        return Optional<repo::SnapshotRecord>();
    }
    repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
        const repo::SnapshotListFilter&) {
        return std::vector<repo::SnapshotRecord>();
    }
    repo::RepositoryResult<Unit> Delete(const std::string&) { return Unit(); }
    repo::RepositoryResult<Optional<std::string> > ResolveAlias(const std::string&) {
        return Optional<std::string>();
    }
    repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(const std::string&) {
        return repo::SnapshotRecord();
    }
    repo::RepositoryResult<Unit> MarkBuildError(const std::string&, const std::string&) {
        return Unit();
    }

    // --- volume half --------------------------------------------------------
    repo::RepositoryResult<Optional<VolumeRecord> > GetVolume(const std::string& reference) {
        // Upstream resolves by id first, then by name alias.
        const std::map<std::string, VolumeRecord>::const_iterator by_id = records_.find(reference);
        if (by_id != records_.end()) return Optional<VolumeRecord>(by_id->second);
        for (std::map<std::string, VolumeRecord>::const_iterator it = records_.begin();
             it != records_.end(); ++it) {
            if (it->second.name == reference) return Optional<VolumeRecord>(it->second);
        }
        return Optional<VolumeRecord>();
    }

    repo::RepositoryResult<repo::VolumeRecordPage> ListVolumesPage(
        const Optional<std::string>& after_volume_id, std::size_t limit) {
        repo::VolumeRecordPage page;
        // `records_` is a std::map, so iteration is already id-ordered, which
        // is what makes the cursor stable.
        std::map<std::string, VolumeRecord>::const_iterator it = records_.begin();
        if (after_volume_id.has_value()) {
            it = records_.upper_bound(*after_volume_id);
        }
        for (; it != records_.end() && page.records.size() < limit; ++it) {
            page.records.push_back(it->second);
        }
        if (it != records_.end()) {
            page.next_volume_id = page.records.back().id;
        }
        return page;
    }

    repo::RepositoryResult<Unit> CreateVolume(const VolumeRecord& record) {
        for (std::map<std::string, VolumeRecord>::const_iterator it = records_.begin();
             it != records_.end(); ++it) {
            if (it->second.name == record.name) {
                return agentenv::core::make_unexpected(
                    repo::RepositoryError::AliasConflict(record.name, it->second.id, record.id));
            }
        }
        records_[record.id] = Persisted(record);
        return Unit();
    }

    repo::RepositoryResult<Unit> PutVolume(const VolumeRecord& record) {
        const std::map<std::string, VolumeRecord>::iterator found = records_.find(record.id);
        if (found == records_.end()) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::SnapshotNotFound(record.id));
        }
        // The durable catalog enforces the same transition rules upstream.
        const agentenv::core::Expected<Unit, std::string> allowed =
            found->second.ValidateCatalogUpdate(record);
        if (!allowed.ok()) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::InvalidRequest(allowed.error()));
        }
        found->second = Persisted(record);
        return Unit();
    }

    repo::RepositoryResult<Unit> DeleteVolume(const std::string& volume_id) {
        if (records_.erase(volume_id) == 0) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::SnapshotNotFound(volume_id));
        }
        return Unit();
    }

    repo::RepositoryResult<Optional<std::string> > ReserveVolume(const std::string& volume_id,
                                                                 const std::string& owner) {
        const std::map<std::string, VolumeRecord>::iterator found = records_.find(volume_id);
        if (found == records_.end()) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::SnapshotNotFound(volume_id));
        }
        if (found->second.reserved_by_sandbox_id.has_value() &&
            *found->second.reserved_by_sandbox_id != owner) {
            return Optional<std::string>(*found->second.reserved_by_sandbox_id);
        }
        found->second.reserved_by_sandbox_id = owner;
        return Optional<std::string>();
    }

    repo::RepositoryResult<Unit> ReserveReadOnlyVolume(const std::string& volume_id,
                                                       const std::string& owner) {
        const std::map<std::string, VolumeRecord>::iterator found = records_.find(volume_id);
        if (found == records_.end()) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::SnapshotNotFound(volume_id));
        }
        std::vector<std::string>& mounts = found->second.read_only_mounts;
        for (std::size_t i = 0; i < mounts.size(); ++i) {
            if (mounts[i] == owner) return Unit();
        }
        mounts.push_back(owner);
        return Unit();
    }

    repo::RepositoryResult<Unit> ReplaceVolumeOwnerFor(const std::string& volume_id,
                                                       const std::string& owner,
                                                       const Optional<std::string>& new_owner) {
        const std::map<std::string, VolumeRecord>::iterator found = records_.find(volume_id);
        if (found == records_.end()) {
            return agentenv::core::make_unexpected(
                repo::RepositoryError::SnapshotNotFound(volume_id));
        }
        found->second.ReplaceOwner(owner, new_owner);
        return Unit();
    }

    /// Mirrors the POSIX backend: every local lower becomes a managed layer
    /// keyed by content digest, while entries that only carry a remote URL are
    /// preserved as external references.
    repo::RepositoryResult<std::vector<OverlaybdLayerRef> > PublishVolumeBacking(
        const std::string&, const std::string& image_config_path) {
        const agentenv::core::Expected<obd::ImageConfig, std::string> loaded =
            obd::LoadImageConfig(image_config_path);
        if (!loaded.ok()) {
            return agentenv::core::make_unexpected(repo::RepositoryError::Backend(loaded.error()));
        }

        std::vector<OverlaybdLayerRef> published;
        for (std::size_t i = 0; i < loaded.value().lowers.size(); ++i) {
            const obd::LayerConfig& layer = loaded.value().lowers[i];
            if (layer.file.empty()) {
                ExternalLayer external;
                external.digest = layer.digest;
                external.repo_blob_url = layer.repo_blob_url;
                external.size = layer.size;
                published.push_back(OverlaybdLayerRef::External(external));
                continue;
            }
            const agentenv::core::Expected<std::string, std::string> bytes =
                agentenv::core::fs::ReadToString(layer.file);
            if (!bytes.ok()) {
                return agentenv::core::make_unexpected(
                    repo::RepositoryError::Backend(bytes.error()));
            }
            ManagedLayer managed;
            managed.digest = agentenv::core::Sha256Digest(bytes.value());
            managed.size = bytes.value().size();
            blobs_[managed.digest] = bytes.value();
            published.push_back(OverlaybdLayerRef::Managed(managed));
        }
        return published;
    }

    repo::RepositoryResult<std::string> MaterializeVolumeBacking(
        const std::string&, const std::vector<OverlaybdLayerRef>& layers,
        const std::string& destination) {
        obd::ImageConfig config;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            obd::LayerConfig layer;
            if (layers[i].is_managed()) {
                const std::map<std::string, std::string>::const_iterator blob =
                    blobs_.find(layers[i].managed.digest);
                if (blob == blobs_.end()) {
                    return agentenv::core::make_unexpected(
                        repo::RepositoryError::Backend("managed layer blob is missing"));
                }
                std::ostringstream name;
                name << "materialized-" << i << ".commit";
                const std::string path =
                    agentenv::core::fs::Join(store_root_, name.str());
                const agentenv::core::Expected<Unit, std::string> created =
                    agentenv::core::fs::CreateDirAll(store_root_);
                if (!created.ok()) {
                    return agentenv::core::make_unexpected(
                        repo::RepositoryError::Backend(created.error()));
                }
                const agentenv::core::Expected<Unit, std::string> written =
                    agentenv::core::fs::Write(path, blob->second);
                if (!written.ok()) {
                    return agentenv::core::make_unexpected(
                        repo::RepositoryError::Backend(written.error()));
                }
                layer.file = path;
                layer.digest = layers[i].managed.digest;
                layer.size = layers[i].managed.size;
            } else {
                // External layers stay by reference: no local file is written.
                layer.repo_blob_url = layers[i].external.repo_blob_url;
                layer.digest = layers[i].external.digest;
                layer.size = layers[i].external.size;
            }
            config.lowers.push_back(layer);
        }
        const agentenv::core::Expected<bool, std::string> saved =
            obd::SaveImageConfig(destination, config);
        if (!saved.ok()) {
            return agentenv::core::make_unexpected(repo::RepositoryError::Backend(saved.error()));
        }
        return destination;
    }

    std::size_t volume_count() const { return records_.size(); }

 private:
    /// `backing_image_config` is `#[serde(skip)]`, so the durable copy must not
    /// keep the node-local path. Dropping it here is what lets the tests prove
    /// a stale local file cannot shadow committed layers.
    static VolumeRecord Persisted(const VolumeRecord& record) {
        VolumeRecord stored = record;
        stored.backing_image_config = Optional<std::string>();
        return stored;
    }

    std::string store_root_;
    std::map<std::string, VolumeRecord> records_;
    std::map<std::string, std::string> blobs_;
};

struct Fixture {
    std::string root;
    std::shared_ptr<FakeVolumeRepository> repository;
    std::shared_ptr<VolumeManager> manager;

    Fixture() {
        const agentenv::core::Expected<std::string, std::string> temp =
            agentenv::core::fs::CreateTempDir("agentenv-volume-");
        MT_EXPECT_TRUE(temp.ok());
        root = temp.value();
        repository.reset(new FakeVolumeRepository(agentenv::core::fs::Join(root, "store")));
        Reopen();
    }
    ~Fixture() { agentenv::core::fs::RemoveDirAll(root); }

    /// Rebuilds the manager while keeping the durable catalog, which is how the
    /// upstream tests simulate a cold node (`manager_at` called twice).
    void Reopen() {
        const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
            VolumeManager::OpenWithRepository(
                agentenv::core::fs::Join(root, "volumes/catalog"), repository);
        MT_EXPECT_TRUE(opened.ok());
        manager = opened.value();
    }

    /// Rust test helper `published_volume`: one local layer plus one external
    /// layer, created from a source image config.
    VolumeRecord PublishedVolume() {
        const std::string layer = agentenv::core::fs::Join(root, "base.commit");
        MT_EXPECT_TRUE(agentenv::core::fs::Write(layer, "published volume layer").ok());

        obd::ImageConfig config;
        obd::LayerConfig local;
        local.file = layer;
        config.lowers.push_back(local);
        obd::LayerConfig external;
        external.repo_blob_url = "https://registry.example.test/v2/cache/blobs";
        external.digest = agentenv::core::Sha256Digest("external volume layer");
        external.size = 21;
        config.lowers.push_back(external);

        const std::string source = agentenv::core::fs::Join(root, "source.json");
        MT_EXPECT_TRUE(obd::SaveImageConfig(source, config).ok());

        const agentenv::volume::VolumeResult<VolumeRecord> created = manager->Create(
            "parent", VolumeMode::Exclusive, Optional<std::string>(),
            Optional<std::string>(source), 1024);
        MT_EXPECT_TRUE(created.ok());
        return created.value();
    }

    /// Rust test helper `append_local_layer`.
    void AppendLocalLayer(const VolumeRecord& record) {
        MT_EXPECT_TRUE(record.backing_image_config.has_value());
        const std::string& path = *record.backing_image_config;
        const Optional<std::string> parent = agentenv::core::fs::Parent(path);
        MT_EXPECT_TRUE(parent.has_value());
        const std::string layer = agentenv::core::fs::Join(*parent, "unpublished.commit");
        MT_EXPECT_TRUE(agentenv::core::fs::Write(layer, "new volume writes").ok());

        const agentenv::core::Expected<obd::ImageConfig, std::string> loaded =
            obd::LoadImageConfig(path);
        MT_EXPECT_TRUE(loaded.ok());
        obd::ImageConfig config = loaded.value();
        obd::LayerConfig appended;
        appended.file = layer;
        config.lowers.push_back(appended);
        MT_EXPECT_TRUE(obd::SaveImageConfig(path, config).ok());
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

MT_TEST(volume_component_validation_matches_rust_rules) {
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent("vol_01234abcXYZ"));
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent("a-b_c"));
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent(std::string(128, 'a')));

    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(""));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(std::string(129, 'a')));
    // Path separators and dots are what make a decoded cursor dangerous.
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("../escape"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has/slash"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has.dot"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has space"));
    // Explicit length: a bare literal would truncate at the NUL and wrongly
    // test the (valid) string "nul".
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(std::string("nul\0byte", 8)));
}

MT_TEST(volume_cursor_round_trips_and_rejects_tampering) {
    const std::string id = "vol_0190f3a9c2b47d5e8f1a2b3c4d5e6f70";
    const std::string token = agentenv::volume::EncodeVolumeCursor(id);
    // base64url, so no '+', '/' or '=' may appear.
    MT_EXPECT_TRUE(token.find('+') == std::string::npos);
    MT_EXPECT_TRUE(token.find('/') == std::string::npos);
    MT_EXPECT_TRUE(token.find('=') == std::string::npos);

    const agentenv::volume::VolumeResult<std::string> decoded =
        agentenv::volume::DecodeVolumeCursor(token);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(decoded.value(), id);

    // Not base64 at all.
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("!!!").error() ==
                   VolumeError::InvalidNextToken());
    // Valid base64 whose payload is a traversal attempt.
    const std::string evil = agentenv::volume::EncodeVolumeCursor("../../etc/passwd");
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor(evil).error() ==
                   VolumeError::InvalidNextToken());
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("").error() ==
                   VolumeError::InvalidNextToken());
}

MT_TEST(volume_cursor_encodes_every_input_length) {
    // Exercises all three tail cases of the base64 encoder (0, 1, 2 leftover
    // bytes), since a wrong tail silently corrupts pagination.
    const char* const inputs[] = {"a", "ab", "abc", "abcd", "abcde", "abcdef"};
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const std::string original = inputs[i];
        const agentenv::volume::VolumeResult<std::string> decoded =
            agentenv::volume::DecodeVolumeCursor(agentenv::volume::EncodeVolumeCursor(original));
        MT_EXPECT_TRUE(decoded.ok());
        MT_EXPECT_EQ(decoded.value(), original);
    }
}

MT_TEST(volume_error_messages_match_thiserror_text) {
    MT_EXPECT_EQ(VolumeError::InvalidName().ToString(),
                 std::string("volume name must contain only letters, numbers, underscores, "
                             "or hyphens"));
    MT_EXPECT_EQ(VolumeError::NameConflict("data").ToString(),
                 std::string("volume name already exists: data"));
    MT_EXPECT_EQ(VolumeError::NotFound("vol_x").ToString(),
                 std::string("volume not found: vol_x"));
    MT_EXPECT_EQ(VolumeError::Reserved("sbx_1").ToString(),
                 std::string("volume is reserved by sandbox sbx_1"));
    MT_EXPECT_EQ(VolumeError::MultipleSources().ToString(),
                 std::string("fromVolume and image cannot be used together"));
    MT_EXPECT_EQ(VolumeError::SizeLimitExceeded(65536).ToString(),
                 std::string("volume size exceeds the configured maximum of 65536 MiB"));
    MT_EXPECT_EQ(VolumeError::TooManyMountedVolumes(24).ToString(),
                 std::string("sandbox cannot mount more than 24 volumes"));
    MT_EXPECT_EQ(VolumeError::Storage("disk on fire").ToString(),
                 std::string("volume catalog storage failed: disk on fire"));

    // Payload-carrying variants must not compare equal across payloads.
    MT_EXPECT_TRUE(VolumeError::SizeLimitExceeded(1) != VolumeError::SizeLimitExceeded(2));
    MT_EXPECT_TRUE(VolumeError::NotFound("a") != VolumeError::NotFound("b"));
    MT_EXPECT_TRUE(VolumeError::SizeMismatch() == VolumeError::SizeMismatch());
}

MT_TEST(replace_owner_moves_exclusive_and_read_only_leases) {
    VolumeRecord record;
    record.id = "vol_1";
    record.reserved_by_sandbox_id = std::string("old");
    MT_EXPECT_TRUE(record.ReplaceOwner("old", Optional<std::string>(std::string("new"))));
    MT_EXPECT_EQ(*record.reserved_by_sandbox_id, std::string("new"));

    // Releasing clears the field entirely.
    MT_EXPECT_TRUE(record.ReplaceOwner("new", Optional<std::string>()));
    MT_EXPECT_TRUE(!record.reserved_by_sandbox_id.has_value());
    // A no-op replacement reports no change.
    MT_EXPECT_TRUE(!record.ReplaceOwner("absent", Optional<std::string>()));

    VolumeRecord shared;
    shared.read_only_mounts.push_back("a");
    shared.read_only_mounts.push_back("b");
    MT_EXPECT_TRUE(shared.ReplaceOwner("a", Optional<std::string>(std::string("b"))));
    // "b" already held a lease, so it must not be duplicated.
    MT_EXPECT_EQ(shared.read_only_mounts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(shared.read_only_mounts[0], std::string("b"));

    MT_EXPECT_TRUE(shared.MountedBy("b"));
    MT_EXPECT_TRUE(!shared.MountedBy("a"));
}

MT_TEST(validate_catalog_update_blocks_illegal_transitions) {
    VolumeRecord current;
    current.id = "vol_1";
    current.name = "data";
    current.size_mb = 1024;
    current.status = VolumeStatus::Ready;

    // Identity fields are immutable.
    VolumeRecord renamed = current;
    renamed.name = "other";
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(renamed).ok());
    VolumeRecord resized = current;
    resized.size_mb = 2048;
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(resized).ok());

    // Replacing the backing requires passing through Uploading first.
    VolumeRecord relayered = current;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 4;
    relayered.backing_layers.push_back(OverlaybdLayerRef::Managed(layer));
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(relayered).ok());
    current.status = VolumeStatus::Uploading;
    MT_EXPECT_TRUE(current.ValidateCatalogUpdate(relayered).ok());

    // Failed may not jump straight back to Ready.
    VolumeRecord failed = current;
    failed.status = VolumeStatus::Failed;
    VolumeRecord ready = failed;
    ready.status = VolumeStatus::Ready;
    MT_EXPECT_TRUE(!failed.ValidateCatalogUpdate(ready).ok());

    // A record being deleted refuses every update.
    VolumeRecord deleting = current;
    deleting.deleting = true;
    MT_EXPECT_TRUE(!deleting.ValidateCatalogUpdate(current).ok());
}

MT_TEST(layer_ref_equality_distinguishes_variants) {
    ManagedLayer managed;
    managed.digest = "sha256:abc";
    managed.size = 10;
    ExternalLayer external;
    external.digest = "sha256:abc";
    external.size = 10;

    // Same digest and size, but different variants are never equal.
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(managed) != OverlaybdLayerRef::External(external));
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(managed) == OverlaybdLayerRef::Managed(managed));

    ManagedLayer with_uuid = managed;
    with_uuid.uuid = std::string("u1");
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(with_uuid) != OverlaybdLayerRef::Managed(managed));
}

// ---------------------------------------------------------------------------
// Manager behaviour
// ---------------------------------------------------------------------------

MT_TEST(open_requires_a_catalog_path_with_a_parent) {
    std::shared_ptr<FakeVolumeRepository> repository(new FakeVolumeRepository("/tmp/unused"));
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepository("catalog", repository);
    MT_EXPECT_TRUE(!opened.ok());
    MT_EXPECT_EQ(opened.error().kind, VolumeErrorKind::Storage);
}

MT_TEST(limits_are_clamped_to_the_firecracker_drive_ceiling) {
    std::shared_ptr<FakeVolumeRepository> repository(new FakeVolumeRepository("/tmp/unused"));
    agentenv::volume::VolumeLimits limits;
    limits.max_size_mb = 4096;
    limits.max_mounts = 1000;  // beyond /dev/vdc..vdz
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepositoryAndLimits("/tmp/aenv/catalog", repository, limits);
    MT_EXPECT_TRUE(opened.ok());
    MT_EXPECT_EQ(opened.value()->limits().max_mounts, agentenv::volume::kMaxVolumeMounts);
    MT_EXPECT_EQ(opened.value()->limits().max_size_mb, static_cast<uint64_t>(4096));
}

MT_TEST(create_validates_name_size_and_sources) {
    Fixture fixture;

    MT_EXPECT_EQ(fixture.manager
                     ->Create("bad name", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::InvalidName());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("zero", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(), 0)
                     .error(),
                 VolumeError::InvalidSize());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("huge", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(),
                              fixture.manager->limits().max_size_mb + 1)
                     .error(),
                 VolumeError::SizeLimitExceeded(fixture.manager->limits().max_size_mb));
    MT_EXPECT_EQ(fixture.manager
                     ->Create("both", VolumeMode::Exclusive,
                              Optional<std::string>(std::string("vol_a")),
                              Optional<std::string>(std::string("/tmp/x.json")), 1024)
                     .error(),
                 VolumeError::MultipleSources());
    // A missing source is reported as SourceNotFound, not NotFound.
    MT_EXPECT_EQ(fixture.manager
                     ->Create("orphan", VolumeMode::Exclusive,
                              Optional<std::string>(std::string("vol_missing")),
                              Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::SourceNotFound("vol_missing"));
}

MT_TEST(create_rejects_duplicate_names) {
    Fixture fixture;
    fixture.PublishedVolume();
    const agentenv::volume::VolumeResult<VolumeRecord> again = fixture.manager->Create(
        "parent", VolumeMode::Exclusive, Optional<std::string>(), Optional<std::string>(), 1024);
    MT_EXPECT_EQ(again.error(), VolumeError::NameConflict("parent"));
}

MT_TEST(published_volume_publishes_local_layers_and_keeps_external_refs) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_EQ(parent.status, VolumeStatus::Ready);
    MT_EXPECT_EQ(parent.backing_layers.size(), static_cast<std::size_t>(2));
    // The local layer became repository-owned, digested by content.
    MT_EXPECT_TRUE(parent.backing_layers[0].is_managed());
    MT_EXPECT_EQ(parent.backing_layers[0].managed.digest,
                 agentenv::core::Sha256Digest("published volume layer"));
    MT_EXPECT_EQ(parent.backing_layers[0].managed.size, static_cast<uint64_t>(22));
    // The remote layer stayed a reference.
    MT_EXPECT_TRUE(parent.backing_layers[1].is_external());
    MT_EXPECT_EQ(parent.backing_layers[1].external.repo_blob_url,
                 std::string("https://registry.example.test/v2/cache/blobs"));
}

MT_TEST(published_volume_clones_survive_parent_deletion_without_local_files) {
    // Rust: `published_volume_clones_survive_parent_deletion_without_local_files`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    // A stale local config must not override the committed backing.
    MT_EXPECT_TRUE(
        agentenv::core::fs::Write(*parent.backing_image_config, "stale config").ok());

    std::vector<VolumeRecord> children;
    const VolumeMode modes[] = {VolumeMode::ReadOnly, VolumeMode::Exclusive};
    for (std::size_t index = 0; index < 2; ++index) {
        if (index == 1) fixture.Reopen();  // cold manager, empty local cache
        std::ostringstream name;
        name << "child-" << index;
        const agentenv::volume::VolumeResult<VolumeRecord> child =
            fixture.manager->Create(name.str(), modes[index],
                                    Optional<std::string>(parent.id),
                                    Optional<std::string>(), 1024);
        MT_EXPECT_TRUE(child.ok());
        // Cloning committed layers copies descriptors, never local files.
        MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(child.value().backing_layers,
                                                          parent.backing_layers));
        MT_EXPECT_EQ(child.value().mode, modes[index]);
        MT_EXPECT_TRUE(!child.value().backing_image_config.has_value());
        MT_EXPECT_TRUE(!agentenv::core::fs::Exists(fixture.manager->DataDir(child.value().id)));
        children.push_back(child.value());
    }

    MT_EXPECT_TRUE(fixture.manager->Remove(parent.id).ok());
    fixture.Reopen();

    for (std::size_t i = 0; i < children.size(); ++i) {
        const agentenv::volume::VolumeResult<VolumeRecord> materialized =
            fixture.manager->MaterializeBacking(children[i].id);
        MT_EXPECT_TRUE(materialized.ok());
        const agentenv::core::Expected<obd::ImageConfig, std::string> config =
            obd::LoadImageConfig(*materialized.value().backing_image_config);
        MT_EXPECT_TRUE(config.ok());
        // The managed layer's bytes survived the parent's deletion.
        const agentenv::core::Expected<std::string, std::string> bytes =
            agentenv::core::fs::ReadToString(config.value().lowers[0].file);
        MT_EXPECT_TRUE(bytes.ok());
        MT_EXPECT_EQ(bytes.value(), std::string("published volume layer"));
        MT_EXPECT_EQ(config.value().lowers[0].digest,
                     agentenv::core::Sha256Digest("published volume layer"));
        MT_EXPECT_EQ(config.value().lowers[0].size, static_cast<uint64_t>(22));
        // The external layer has no local file, only its URL.
        MT_EXPECT_TRUE(config.value().lowers[1].file.empty());
        MT_EXPECT_EQ(config.value().lowers[1].repo_blob_url,
                     std::string("https://registry.example.test/v2/cache/blobs"));
    }
}

MT_TEST(published_volume_clone_validates_source_state_and_size) {
    // Rust: `published_volume_clone_validates_source_state_and_size`.
    Fixture fixture;
    VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_EQ(fixture.manager
                     ->Create("wrong-size", VolumeMode::ReadOnly,
                              Optional<std::string>(parent.id), Optional<std::string>(), 2048)
                     .error(),
                 VolumeError::SizeMismatch());

    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("reserved", VolumeMode::ReadOnly,
                              Optional<std::string>(parent.id), Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::Reserved("owner"));

    // Uploading and Failed sources are both unusable, with distinct errors.
    const VolumeStatus statuses[] = {VolumeStatus::Uploading, VolumeStatus::Failed};
    for (std::size_t i = 0; i < 2; ++i) {
        parent.status = statuses[i];
        MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
        const agentenv::volume::VolumeResult<VolumeRecord> unready = fixture.manager->Create(
            "unready", VolumeMode::ReadOnly, Optional<std::string>(parent.id),
            Optional<std::string>(), 1024);
        const VolumeError expected = statuses[i] == VolumeStatus::Uploading
                                         ? VolumeError::Uploading(parent.id)
                                         : VolumeError::Failed(parent.id);
        MT_EXPECT_EQ(unready.error(), expected);
    }
}

MT_TEST(reserved_volume_clone_retains_unpublished_local_layers) {
    // Rust: `reserved_volume_clone_retains_unpublished_local_layers`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "source").ok());
    fixture.AppendLocalLayer(parent);

    const agentenv::volume::VolumeResult<VolumeRecord> child =
        fixture.manager->CreateChildForOwner(parent.id, "fork", VolumeMode::Exclusive, 1024,
                                             "source", "child");
    MT_EXPECT_TRUE(child.ok());
    // A reserved parent may hold newer local writes, so the child clones files
    // instead of inheriting descriptors.
    MT_EXPECT_TRUE(child.value().backing_layers.empty());
    MT_EXPECT_TRUE(child.value().backing_image_config.has_value());

    std::vector<std::string> parent_ids;
    parent_ids.push_back(parent.id);
    MT_EXPECT_TRUE(
        fixture.manager->ReplaceOwnerFor("source", Optional<std::string>(), parent_ids).ok());
    MT_EXPECT_TRUE(fixture.manager->Remove(parent.id).ok());

    std::vector<std::string> child_ids;
    child_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(fixture.manager->RecoverAndPublishBackings("child", child_ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> published =
        fixture.manager->Get(child.value().id);
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_EQ(published.value().backing_layers.size(), static_cast<std::size_t>(3));
    ManagedLayer expected;
    expected.digest = agentenv::core::Sha256Digest("new volume writes");
    expected.size = 17;
    MT_EXPECT_TRUE(published.value().backing_layers[2] == OverlaybdLayerRef::Managed(expected));
}

MT_TEST(volume_generations_preserve_inherited_descriptors_and_new_writes) {
    // Rust: `volume_generations_preserve_inherited_descriptors_and_new_writes`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    const agentenv::volume::VolumeResult<VolumeRecord> seed = fixture.manager->Create(
        "seed", VolumeMode::ReadOnly, Optional<std::string>(parent.id), Optional<std::string>(),
        1024);
    MT_EXPECT_TRUE(seed.ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(seed.value().id, "builder").ok());

    const agentenv::volume::VolumeResult<VolumeRecord> child =
        fixture.manager->CreateChildForOwner(seed.value().id, "work", VolumeMode::Exclusive, 1024,
                                             "builder", "builder");
    MT_EXPECT_TRUE(child.ok());
    // The seed holds no local writes, so the child inherits descriptors.
    MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(child.value().backing_layers,
                                                      parent.backing_layers));

    const agentenv::volume::VolumeResult<VolumeRecord> materialized =
        fixture.manager->MaterializeBacking(child.value().id);
    MT_EXPECT_TRUE(materialized.ok());
    fixture.AppendLocalLayer(materialized.value());

    std::vector<std::string> child_ids;
    child_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(fixture.manager->RecoverAndPublishBackings("builder", child_ids).ok());

    // The builder now releases both leases; without this the volumes stay
    // reserved and cannot seed a further generation.
    std::vector<std::string> builder_ids;
    builder_ids.push_back(seed.value().id);
    builder_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(
        fixture.manager->ReplaceOwnerFor("builder", Optional<std::string>(), builder_ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> published =
        fixture.manager->Get(child.value().id);
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_EQ(published.value().backing_layers.size(), static_cast<std::size_t>(3));
    // Inherited descriptors are byte-identical; only the new write is appended.
    MT_EXPECT_TRUE(published.value().backing_layers[0] == parent.backing_layers[0]);
    MT_EXPECT_TRUE(published.value().backing_layers[1] == parent.backing_layers[1]);
    ManagedLayer expected;
    expected.digest = agentenv::core::Sha256Digest("new volume writes");
    expected.size = 17;
    MT_EXPECT_TRUE(published.value().backing_layers[2] == OverlaybdLayerRef::Managed(expected));

    // The next generation seeds from the published child without local files.
    const agentenv::volume::VolumeResult<VolumeRecord> next_seed = fixture.manager->Create(
        "next-seed", VolumeMode::ReadOnly, Optional<std::string>(child.value().id),
        Optional<std::string>(), 1024);
    MT_EXPECT_TRUE(next_seed.ok());
    MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(next_seed.value().backing_layers,
                                                      published.value().backing_layers));
    MT_EXPECT_TRUE(!next_seed.value().backing_image_config.has_value());
}

MT_TEST(create_from_snapshot_preserves_read_only_mode) {
    // Rust: `create_from_snapshot_preserves_read_only_mode`.
    Fixture fixture;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 4096;
    std::vector<OverlaybdLayerRef> layers;
    layers.push_back(OverlaybdLayerRef::Managed(layer));

    const agentenv::volume::VolumeResult<VolumeRecord> restored =
        fixture.manager->CreateFromSnapshot("restored-read-only", VolumeMode::ReadOnly, 1024,
                                            layers);
    MT_EXPECT_TRUE(restored.ok());
    MT_EXPECT_EQ(restored.value().mode, VolumeMode::ReadOnly);

    const agentenv::volume::VolumeResult<VolumeRecord> persisted =
        fixture.manager->Get(restored.value().id);
    MT_EXPECT_TRUE(persisted.ok());
    MT_EXPECT_EQ(persisted.value().mode, VolumeMode::ReadOnly);

    // An empty layer set is not a restorable snapshot.
    MT_EXPECT_EQ(fixture.manager
                     ->CreateFromSnapshot("empty", VolumeMode::ReadOnly, 1024,
                                          std::vector<OverlaybdLayerRef>())
                     .error(),
                 VolumeError::InvalidSize());
}

MT_TEST(delete_refuses_while_any_lease_is_held) {
    Fixture fixture;
    const VolumeRecord exclusive = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(exclusive.id, "owner").ok());
    MT_EXPECT_EQ(fixture.manager->Remove(exclusive.id).error(), VolumeError::Reserved("owner"));

    // Read-only mounts block deletion too.
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    const agentenv::volume::VolumeResult<VolumeRecord> shared =
        fixture.manager->CreateFromSnapshot("shared", VolumeMode::ReadOnly, 1024, layers);
    MT_EXPECT_TRUE(shared.ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "reader").ok());
    MT_EXPECT_EQ(fixture.manager->Remove(shared.value().id).error(),
                 VolumeError::Reserved("reader"));

    // Releasing the lease unblocks it.
    std::vector<std::string> ids;
    ids.push_back(shared.value().id);
    MT_EXPECT_TRUE(fixture.manager->ReplaceOwnerFor("reader", Optional<std::string>(), ids).ok());
    MT_EXPECT_TRUE(fixture.manager->Remove(shared.value().id).ok());
    MT_EXPECT_EQ(fixture.manager->Get(shared.value().id).error(),
                 VolumeError::NotFound(shared.value().id));
}

MT_TEST(reserve_rejects_unusable_states_and_conflicting_owners) {
    Fixture fixture;
    VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "first").ok());
    // A second exclusive reservation reports the incumbent.
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "second").error(),
                 VolumeError::Reserved("first"));

    parent.status = VolumeStatus::Uploading;
    MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "third").error(),
                 VolumeError::Uploading(parent.id));

    parent.status = VolumeStatus::Failed;
    MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "third").error(),
                 VolumeError::Failed(parent.id));
}

MT_TEST(read_only_reservations_accumulate_and_are_idempotent) {
    Fixture fixture;
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    const agentenv::volume::VolumeResult<VolumeRecord> shared =
        fixture.manager->CreateFromSnapshot("shared", VolumeMode::ReadOnly, 1024, layers);
    MT_EXPECT_TRUE(shared.ok());

    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "a").ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "b").ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "a").ok());  // idempotent

    const agentenv::volume::VolumeResult<VolumeRecord> reloaded =
        fixture.manager->Get(shared.value().id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_EQ(reloaded.value().read_only_mounts.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(reloaded.value().MountedBy("a"));
    MT_EXPECT_TRUE(reloaded.value().MountedBy("b"));
    // A read-only volume never takes an exclusive owner.
    MT_EXPECT_TRUE(!reloaded.value().reserved_by_sandbox_id.has_value());
}

MT_TEST(fail_backings_only_touches_volumes_owned_by_the_caller) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    // A different owner must not be able to fail someone else's volume.
    MT_EXPECT_TRUE(fixture.manager->FailBackings("stranger", ids).ok());
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Ready);

    MT_EXPECT_TRUE(fixture.manager->FailBackings("owner", ids).ok());
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Failed);
}

MT_TEST(recover_backings_fails_volume_whose_local_backing_vanished) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    // Simulate a restart that lost the node-local directory.
    MT_EXPECT_TRUE(
        agentenv::core::fs::RemoveDirAll(fixture.manager->DataDir(parent.id)).ok());
    fixture.Reopen();

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    const agentenv::core::Expected<Unit, VolumeError> recovered =
        fixture.manager->RecoverBackings("owner", ids);
    MT_EXPECT_TRUE(!recovered.ok());
    MT_EXPECT_EQ(recovered.error().kind, VolumeErrorKind::Storage);
    // The volume is marked Failed so nobody mounts stale content.
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Failed);
}

MT_TEST(recover_backings_restores_the_deterministic_local_path) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    // A cold manager has no local cache, but the file is still on disk.
    fixture.Reopen();
    MT_EXPECT_TRUE(!fixture.manager->Get(parent.id).value().backing_image_config.has_value());

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    MT_EXPECT_TRUE(fixture.manager->RecoverBackings("owner", ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> recovered =
        fixture.manager->Get(parent.id);
    MT_EXPECT_TRUE(recovered.value().backing_image_config.has_value());
    MT_EXPECT_EQ(*recovered.value().backing_image_config,
                 agentenv::core::fs::Join(fixture.manager->DataDir(parent.id), "image.json"));
}

MT_TEST(list_page_paginates_and_validates_its_cursor) {
    Fixture fixture;
    // Snapshot-restored volumes need no external tooling, so they are the
    // cheapest way to fill a catalog.
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    for (int i = 0; i < 5; ++i) {
        std::ostringstream name;
        name << "vol-" << i;
        MT_EXPECT_TRUE(
            fixture.manager->CreateFromSnapshot(name.str(), VolumeMode::ReadOnly, 1024, layers)
                .ok());
    }

    MT_EXPECT_EQ(fixture.manager->ListPage(Optional<std::string>(), 0).error(),
                 VolumeError::InvalidPageLimit());
    MT_EXPECT_EQ(
        fixture.manager->ListPage(Optional<std::string>(std::string("!!!")), 2).error(),
        VolumeError::InvalidNextToken());

    // Walk every page and make sure each record appears exactly once.
    std::vector<std::string> seen;
    Optional<std::string> cursor;
    for (int guard = 0; guard < 10; ++guard) {
        const agentenv::volume::VolumeResult<agentenv::volume::VolumePage> page =
            fixture.manager->ListPage(cursor, 2);
        MT_EXPECT_TRUE(page.ok());
        for (std::size_t i = 0; i < page.value().records.size(); ++i) {
            seen.push_back(page.value().records[i].id);
        }
        if (!page.value().next_token.has_value()) break;
        cursor = page.value().next_token;
    }
    MT_EXPECT_EQ(seen.size(), static_cast<std::size_t>(5));
    for (std::size_t i = 1; i < seen.size(); ++i) {
        MT_EXPECT_TRUE(seen[i - 1] < seen[i]);  // strictly increasing: no repeats
    }
}

MT_TEST(get_reports_not_found_for_unknown_references) {
    Fixture fixture;
    MT_EXPECT_EQ(fixture.manager->Get("vol_missing").error(),
                 VolumeError::NotFound("vol_missing"));
    // Lookups resolve by name as well as by id.
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_EQ(fixture.manager->Get("parent").value().id, parent.id);
}

MT_TEST(unsupported_repository_reports_the_upstream_feature_names) {
    // A backend that implements only the snapshot half must fail loudly.
    class SnapshotOnlyRepository : public repo::SnapshotRepository {
     public:
        repo::RepositoryResult<repo::SnapshotRecord> Create(const repo::SnapshotRecord& r) {
            return r;
        }
        repo::RepositoryResult<Optional<repo::SnapshotRecord> > Get(const std::string&) {
            return Optional<repo::SnapshotRecord>();
        }
        repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
            const repo::SnapshotListFilter&) {
            return std::vector<repo::SnapshotRecord>();
        }
        repo::RepositoryResult<Unit> Delete(const std::string&) { return Unit(); }
        repo::RepositoryResult<Optional<std::string> > ResolveAlias(const std::string&) {
            return Optional<std::string>();
        }
        repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(const std::string&) {
            return repo::SnapshotRecord();
        }
        repo::RepositoryResult<Unit> MarkBuildError(const std::string&, const std::string&) {
            return Unit();
        }
    };

    std::shared_ptr<repo::SnapshotRepository> repository(new SnapshotOnlyRepository());
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepository("/tmp/aenv/catalog", repository);
    MT_EXPECT_TRUE(opened.ok());

    const agentenv::volume::VolumeResult<VolumeRecord> got = opened.value()->Get("anything");
    MT_EXPECT_EQ(got.error().kind, VolumeErrorKind::Storage);
    MT_EXPECT_TRUE(got.error().detail.find("volume catalog") != std::string::npos);
}

int main() { return microtest::RunAll(); }
