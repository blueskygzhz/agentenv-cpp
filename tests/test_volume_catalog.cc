// SPDX-License-Identifier: MIT
// Rust: src/volume/catalog.rs test module.
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/volume/catalog.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
using agentenv::volume::VolumeMode;
using agentenv::volume::VolumeRecord;
using agentenv::volume::VolumeRecordPage;
using agentenv::volume::VolumeStatus;
namespace fs = agentenv::core::fs;
namespace repo = agentenv::snapshot::repository;
namespace volume = agentenv::volume;

struct TempDir {
    std::string path;
    TempDir() { path = fs::CreateTempDir("agentenv-volume-catalog-").value(); }
    ~TempDir() { fs::RemoveDirAll(path); }
};

/// Rust test helper `repository` — a POSIX backend rooted at
/// `root/repository`.
std::shared_ptr<repo::SnapshotRepository> Repository(const std::string& root) {
    return std::make_shared<repo::posixfs::PosixFsSnapshotRepository>(
        std::make_shared<repo::posixfs::PosixFsCatalogStore>(fs::Join(root, "repository")));
}

std::string Padded(std::size_t index) {
    std::ostringstream oss;
    oss.width(6);
    oss.fill('0');
    oss << index;
    return oss.str();
}

/// Rust test helper `volume_record`.
VolumeRecord MakeVolumeRecord(std::size_t index) {
    VolumeRecord record;
    record.id = "vol_" + Padded(index);
    record.name = "data-" + Padded(index);
    record.mode = VolumeMode::Exclusive;
    record.size_mb = volume::kDefaultVolumeSizeMb;
    record.status = VolumeStatus::Ready;
    return record;
}

/// Hides one volume record from reads but not from listings, like a delete
/// that lands between the two.
class DeletedAfterListing : public repo::SnapshotRepository {
 public:
    DeletedAfterListing(std::shared_ptr<repo::SnapshotRepository> inner, const std::string& hidden)
        : inner_(inner), hidden_(hidden) {}

    repo::RepositoryResult<repo::SnapshotRecord> Create(const repo::SnapshotRecord& r) override {
        return r;
    }
    repo::RepositoryResult<Optional<repo::SnapshotRecord> > Get(const std::string&) override {
        return Optional<repo::SnapshotRecord>();
    }
    repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
        const repo::SnapshotListFilter&) override {
        return std::vector<repo::SnapshotRecord>();
    }
    repo::RepositoryResult<Unit> Delete(const std::string&) override { return Unit(); }
    repo::RepositoryResult<Optional<agentenv::core::SnapshotId> > ResolveAlias(
        const std::string&) override {
        return Optional<agentenv::core::SnapshotId>();
    }
    repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(
        const agentenv::core::SnapshotId&) override {
        return repo::SnapshotRecord();
    }
    repo::RepositoryResult<Unit> MarkBuildError(
        const agentenv::core::SnapshotId&,
        const agentenv::snapshot::TemplateBuildErrorReason&) override {
        return Unit();
    }

    repo::RepositoryResult<Optional<std::string> > GetDocument(
        const repo::DocumentKey& key) override {
        if (key.kind() == repo::DocumentKey::Kind::VolumeRecord &&
            key.Path() == repo::DocumentKey::VolumeRecord(hidden_).Path()) {
            return Optional<std::string>();
        }
        return inner_->GetDocument(key);
    }
    repo::RepositoryResult<std::vector<std::string> > ListDocuments(
        repo::DocumentCollection collection, const Optional<std::string>& after,
        std::size_t limit) override {
        return inner_->ListDocuments(collection, after, limit);
    }

 private:
    std::shared_ptr<repo::SnapshotRepository> inner_;
    std::string hidden_;
};

}  // namespace

MT_TEST(posix_catalog_keeps_existing_layout_and_locks) {
    TempDir temp;
    std::shared_ptr<repo::SnapshotRepository> repository = Repository(temp.path);
    std::vector<std::string> expected_ids;
    for (std::size_t index = 0; index < 40; ++index) {
        const VolumeRecord record = MakeVolumeRecord(index);
        expected_ids.push_back(record.id);
        MT_EXPECT_TRUE(volume::CreateVolume(*repository, record).ok());
    }
    MT_EXPECT_EQ(volume::GetVolume(*repository, "data-000012").value()->id,
                 std::string("vol_000012"));
    VolumeRecord conflict = MakeVolumeRecord(12);
    conflict.id = "vol_conflict";
    const repo::RepositoryResult<Unit> conflicted = volume::CreateVolume(*repository, conflict);
    MT_EXPECT_TRUE(!conflicted.ok());
    MT_EXPECT_TRUE(conflicted.error().kind == repo::RepositoryErrorKind::VolumeNameConflict);
    MT_EXPECT_TRUE(!volume::GetVolume(*repository, "vol_conflict").value().has_value());
    const std::string root = fs::Join(temp.path, "repository");
    const char* paths[] = {
        "volumes/records/vol_000012.json",
        "volumes/records/vol_000012.lock",
        "volumes/aliases/data-000012",
        "volumes/aliases/data-000012.lock",
    };
    for (std::size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        MT_EXPECT_TRUE(fs::IsFile(fs::Join(root, paths[i])));
    }

    std::vector<std::string> actual_ids;
    Optional<std::string> after;
    for (;;) {
        const repo::RepositoryResult<VolumeRecordPage> page =
            volume::ListVolumesPage(*repository, after, 7);
        MT_EXPECT_TRUE(page.ok());
        for (std::size_t i = 0; i < page.value().records.size(); ++i) {
            actual_ids.push_back(page.value().records[i].id);
        }
        if (!page.value().next_volume_id.has_value()) break;
        after = page.value().next_volume_id;
    }
    MT_EXPECT_TRUE(actual_ids == expected_ids);

    const std::string& id = expected_ids[0];
    MT_EXPECT_TRUE(!volume::ReserveVolume(*repository, id, "sandbox-a").value().has_value());
    MT_EXPECT_EQ(*volume::ReserveVolume(*repository, id, "sandbox-b").value(),
                 std::string("sandbox-a"));
    MT_EXPECT_TRUE(volume::ReplaceVolumeOwnerFor(*repository, id, "sandbox-a",
                                                 Optional<std::string>("sandbox-b"))
                       .ok());
    const VolumeRecord record = *volume::GetVolume(*repository, id).value();
    MT_EXPECT_EQ(*record.reserved_by_sandbox_id, std::string("sandbox-b"));
    MT_EXPECT_TRUE(!volume::DeleteVolume(*repository, id).ok());
}

MT_TEST(listing_skips_volumes_deleted_since_the_listing) {
    TempDir temp;
    std::shared_ptr<repo::SnapshotRepository> inner = Repository(temp.path);
    for (std::size_t index = 0; index < 3; ++index) {
        MT_EXPECT_TRUE(volume::CreateVolume(*inner, MakeVolumeRecord(index)).ok());
    }
    DeletedAfterListing repository(inner, "vol_000001");
    repo::RepositoryResult<VolumeRecordPage> page =
        volume::ListVolumesPage(repository, Optional<std::string>(), 2);
    MT_EXPECT_TRUE(page.value().records == std::vector<VolumeRecord>(1, MakeVolumeRecord(0)));
    MT_EXPECT_EQ(*page.value().next_volume_id, std::string("vol_000001"));
    page = volume::ListVolumesPage(repository, page.value().next_volume_id, 2);
    MT_EXPECT_TRUE(page.value().records == std::vector<VolumeRecord>(1, MakeVolumeRecord(2)));
    MT_EXPECT_TRUE(!page.value().next_volume_id.has_value());
}

MT_TEST(concurrent_creates_of_one_name_leave_one_volume) {
    TempDir temp;
    const std::size_t kCreates = 16;
    // Rust collects `Result`s from the tasks and asserts on the test thread;
    // the outcome codes here do the same: 1 created, 0 name conflict, -1 other.
    std::vector<int> outcomes(kCreates, -1);
    std::vector<std::string> errors(kCreates);
    std::vector<std::thread> creates;
    for (std::size_t index = 0; index < kCreates; ++index) {
        creates.push_back(std::thread([&temp, &outcomes, &errors, index]() {
            std::shared_ptr<repo::SnapshotRepository> repository = Repository(temp.path);
            VolumeRecord record = MakeVolumeRecord(index);
            record.name = "shared";
            const repo::RepositoryResult<Unit> created = volume::CreateVolume(*repository, record);
            if (created.ok()) {
                outcomes[index] = 1;
            } else if (created.error().kind == repo::RepositoryErrorKind::VolumeNameConflict) {
                outcomes[index] = 0;
            } else {
                errors[index] = created.error().ToString();
            }
        }));
    }
    std::vector<std::string> created;
    for (std::size_t index = 0; index < kCreates; ++index) {
        creates[index].join();
        if (outcomes[index] < 0) {
            std::fprintf(stderr, "unexpected create error: %s\n", errors[index].c_str());
        }
        MT_EXPECT_TRUE(outcomes[index] >= 0);
        if (outcomes[index] == 1) created.push_back(MakeVolumeRecord(index).id);
    }
    MT_EXPECT_EQ(created.size(), static_cast<std::size_t>(1));
    std::shared_ptr<repo::SnapshotRepository> repository = Repository(temp.path);
    MT_EXPECT_EQ(volume::GetVolume(*repository, "shared").value()->id, created[0]);
    const repo::RepositoryResult<VolumeRecordPage> page =
        volume::ListVolumesPage(*repository, Optional<std::string>(), 100);
    std::vector<std::string> listed;
    for (std::size_t i = 0; i < page.value().records.size(); ++i) {
        listed.push_back(page.value().records[i].id);
    }
    MT_EXPECT_TRUE(listed == created);
}

MT_TEST(reservations_require_a_live_ready_volume) {
    TempDir temp;
    std::shared_ptr<repo::SnapshotRepository> repository = Repository(temp.path);
    VolumeRecord record = MakeVolumeRecord(0);
    MT_EXPECT_TRUE(volume::CreateVolume(*repository, record).ok());
    const VolumeStatus statuses[] = {VolumeStatus::Uploading, VolumeStatus::Failed};
    for (std::size_t i = 0; i < 2; ++i) {
        record.status = statuses[i];
        MT_EXPECT_TRUE(volume::PutVolume(*repository, record).ok());
        MT_EXPECT_TRUE(!volume::ReserveVolume(*repository, record.id, "sandbox").ok());
    }
    MT_EXPECT_TRUE(!volume::ReserveVolume(*repository, "vol_missing", "sandbox").ok());
    MT_EXPECT_TRUE(volume::ReplaceVolumeOwnerFor(*repository, "vol_missing", "sandbox",
                                                 Optional<std::string>())
                       .ok());

    // A volume that is being deleted takes no new owners, and its name can be
    // claimed by a new volume.
    VolumeRecord deleting = MakeVolumeRecord(1);
    deleting.reserved_by_sandbox_id = Optional<std::string>("old");
    MT_EXPECT_TRUE(volume::CreateVolume(*repository, deleting).ok());
    deleting.deleting = true;
    MT_EXPECT_TRUE(repository
                       ->PutDocument(repo::DocumentKey::VolumeRecord(deleting.id),
                                     volume::EncodeVolumeRecord(deleting).value())
                       .ok());
    MT_EXPECT_TRUE(!volume::ReserveVolume(*repository, deleting.id, "new").ok());
    MT_EXPECT_TRUE(volume::ReplaceVolumeOwnerFor(*repository, deleting.id, "old",
                                                 Optional<std::string>("new"))
                       .ok());
    const VolumeRecord unchanged = *volume::GetVolume(*repository, deleting.id).value();
    MT_EXPECT_EQ(*unchanged.reserved_by_sandbox_id, std::string("old"));
    VolumeRecord reclaimed = MakeVolumeRecord(2);
    reclaimed.name = deleting.name;
    MT_EXPECT_TRUE(volume::CreateVolume(*repository, reclaimed).ok());
    MT_EXPECT_EQ(volume::GetVolume(*repository, deleting.name).value()->id,
                 MakeVolumeRecord(2).id);
}

MT_TEST(names_pointing_at_missing_records_are_reclaimed) {
    TempDir temp;
    std::shared_ptr<repo::SnapshotRepository> repository = Repository(temp.path);
    MT_EXPECT_TRUE(
        repository->PutDocument(repo::DocumentKey::VolumeAlias("data"), "\"vol_gone\"").ok());
    MT_EXPECT_TRUE(!volume::GetVolume(*repository, "data").value().has_value());
    VolumeRecord record = MakeVolumeRecord(0);
    record.name = "data";
    MT_EXPECT_TRUE(volume::CreateVolume(*repository, record).ok());
    MT_EXPECT_EQ(volume::GetVolume(*repository, "data").value()->id, record.id);
    MT_EXPECT_TRUE(volume::DeleteVolume(*repository, record.id).ok());
    MT_EXPECT_TRUE(!volume::GetVolume(*repository, "data").value().has_value());
    MT_EXPECT_TRUE(!fs::Exists(fs::Join(temp.path, "repository/volumes/aliases/data")));
}

int main() { return microtest::RunAll(); }
