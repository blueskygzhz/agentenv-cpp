// SPDX-License-Identifier: MIT
// Shared fakes for the volume tests.
//
// Extracted from test_volume.cc so the API-layer volume tests can drive a real
// `VolumeManager` instead of duplicating a 200-line repository fake. Rust gets
// this for free by having both test modules in the same crate.
#ifndef AGENTENV_TESTS_VOLUME_FAKES_H_
#define AGENTENV_TESTS_VOLUME_FAKES_H_

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/volume.h"
#include "microtest.h"

namespace agentenv {
namespace testing {

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

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_VOLUME_FAKES_H_
