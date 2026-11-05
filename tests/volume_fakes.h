// SPDX-License-Identifier: MIT
// Shared fakes for the volume tests.
//
// Extracted from test_volume.cc so the API-layer volume tests can drive a real
// `VolumeManager` over the same repository. Rust gets this for free by having
// both test modules in the same crate.
#ifndef AGENTENV_TESTS_VOLUME_FAKES_H_
#define AGENTENV_TESTS_VOLUME_FAKES_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
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

/// The Rust tests' `PosixFsBackend::new(root.join("store"))`: a real POSIX
/// repository, so the catalog, the layer store and the materializer under
/// test are the production ones. The only addition is a create cutoff.
///
/// A fork creates one clone per child, and the behaviour worth testing is a
/// failure *partway through* — the clones already made have to be cleaned up.
/// Clone names carry a fresh uuid, so a name collision cannot be staged in
/// advance; counting record inserts is the only way in from outside.
class FakeVolumeRepository : public repo::SnapshotRepository {
 public:
    explicit FakeVolumeRepository(const std::string& store_root)
        : inner_(std::make_shared<repo::posixfs::PosixFsCatalogStore>(store_root),
                 std::make_shared<repo::posixfs::PosixFsArtifactStore>(store_root)) {}

    /// Volume records created so far, and an optional cutoff.
    int create_volume_calls = 0;
    /// Refuse once this many creates have succeeded. Negative never refuses.
    int fail_create_volume_after = -1;

    // --- snapshot half: delegated ------------------------------------------
    repo::RepositoryResult<repo::SnapshotRecord> Create(
        const repo::SnapshotRecord& record) override {
        return inner_.Create(record);
    }
    repo::RepositoryResult<Optional<repo::SnapshotRecord> > Get(
        const std::string& id_or_alias) override {
        return inner_.Get(id_or_alias);
    }
    repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
        const repo::SnapshotListFilter& filter) override {
        return inner_.List(filter);
    }
    repo::RepositoryResult<Unit> Delete(const std::string& id_or_alias) override {
        return inner_.Delete(id_or_alias);
    }
    repo::RepositoryResult<Optional<agentenv::core::SnapshotId> > ResolveAlias(
        const std::string& alias) override {
        return inner_.ResolveAlias(alias);
    }
    repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(
        const agentenv::core::SnapshotId& id) override {
        return inner_.TryStartBuild(id);
    }
    repo::RepositoryResult<Unit> MarkBuildError(
        const agentenv::core::SnapshotId& id,
        const agentenv::snapshot::TemplateBuildErrorReason& reason) override {
        return inner_.MarkBuildError(id, reason);
    }

    // --- documents: delegated, record inserts counted ----------------------
    repo::RepositoryResult<Optional<std::string> > GetDocument(
        const repo::DocumentKey& key) override {
        return inner_.GetDocument(key);
    }
    repo::RepositoryResult<Unit> PutDocument(const repo::DocumentKey& key,
                                             const std::string& bytes) override {
        return inner_.PutDocument(key, bytes);
    }
    repo::RepositoryResult<Optional<std::string> > UpdateDocument(
        const repo::DocumentKey& key, const repo::DocumentUpdate& update) override {
        if (key.kind() != repo::DocumentKey::Kind::VolumeRecord) {
            return inner_.UpdateDocument(key, update);
        }
        const repo::RepositoryResult<Optional<std::string> > before = inner_.GetDocument(key);
        if (!before.ok()) return before;
        if (before.value().has_value()) return inner_.UpdateDocument(key, update);
        // A missing record being written is `create_volume`'s insert.
        if (fail_create_volume_after >= 0 && create_volume_calls >= fail_create_volume_after) {
            ++create_volume_calls;
            return agentenv::core::make_unexpected(
                repo::RepositoryError::InvalidRequest("injected create failure"));
        }
        const repo::RepositoryResult<Optional<std::string> > applied =
            inner_.UpdateDocument(key, update);
        if (applied.ok() && !applied.value().has_value()) ++create_volume_calls;
        return applied;
    }
    repo::RepositoryResult<Unit> DeleteDocument(const repo::DocumentKey& key) override {
        return inner_.DeleteDocument(key);
    }
    repo::RepositoryResult<std::vector<std::string> > ListDocuments(
        repo::DocumentCollection collection, const Optional<std::string>& after,
        std::size_t limit) override {
        return inner_.ListDocuments(collection, after, limit);
    }

    // --- blobs: delegated --------------------------------------------------
    Optional<std::string> ManagedLayersRepoBlobUrl() const override {
        return inner_.ManagedLayersRepoBlobUrl();
    }
    repo::RepositoryResult<Unit> PutBlob(const repo::BlobKey& key,
                                         const repo::BlobBody& body) override {
        return inner_.PutBlob(key, body);
    }
    repo::RepositoryResult<repo::BlobLocation> LocateBlob(
        const repo::BlobKey& key) const override {
        return inner_.LocateBlob(key);
    }
    repo::RepositoryResult<bool> HasBlob(const repo::BlobKey& key) override {
        return inner_.HasBlob(key);
    }
    repo::RepositoryResult<std::string> ReadBlob(const repo::BlobKey& key) override {
        return inner_.ReadBlob(key);
    }
    repo::RepositoryResult<uint64_t> FetchBlob(const repo::BlobKey& key,
                                               const std::string& destination) override {
        return inner_.FetchBlob(key, destination);
    }

    /// Durable volume records, deleting ones included.
    std::size_t volume_count() {
        const repo::RepositoryResult<std::vector<std::string> > ids = inner_.ListDocuments(
            repo::DocumentCollection::VolumeRecords, Optional<std::string>(),
            static_cast<std::size_t>(-1));
        MT_EXPECT_TRUE(ids.ok());
        return ids.ok() ? ids.value().size() : 0;
    }

 private:
    repo::posixfs::PosixFsSnapshotRepository inner_;
};

/// Named `VolumeFixture` rather than `Fixture` because `orchestrator_fakes.h`
/// declares a `Fixture` in this same namespace, and a test that needs both a
/// real `VolumeManager` and an orchestrator has to include both headers. The
/// short alias below keeps the existing volume tests reading as they did.
struct VolumeFixture {
    std::string root;
    std::shared_ptr<FakeVolumeRepository> repository;
    std::shared_ptr<VolumeManager> manager;

    VolumeFixture() {
        const agentenv::core::Expected<std::string, std::string> temp =
            agentenv::core::fs::CreateTempDir("agentenv-volume-");
        MT_EXPECT_TRUE(temp.ok());
        root = temp.value();
        repository.reset(new FakeVolumeRepository(agentenv::core::fs::Join(root, "store")));
        Reopen();
    }
    ~VolumeFixture() { agentenv::core::fs::RemoveDirAll(root); }

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

/// The short name the existing volume tests use. Defined only when the
/// orchestrator fakes are not also in play, since both headers would otherwise
/// declare it -- a test needing a real `VolumeManager` *and* an orchestrator
/// includes both and names `VolumeFixture` explicitly.
#ifndef AGENTENV_TESTS_ORCHESTRATOR_FAKES_H_
typedef VolumeFixture Fixture;
#endif

}  // namespace testing
}  // namespace agentenv
#endif  // AGENTENV_TESTS_VOLUME_FAKES_H_
