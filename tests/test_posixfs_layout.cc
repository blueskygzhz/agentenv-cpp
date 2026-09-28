// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/layout.rs, plus the
// `core::file_lock` primitives the POSIX catalog needs.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "agentenv/core/file_lock.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "microtest.h"

namespace {

using agentenv::core::FileLockGuard;
using agentenv::core::Optional;
using agentenv::core::SnapshotId;
using agentenv::core::Unit;
using agentenv::core::Uuid;
namespace fs = agentenv::core::fs;
namespace posixfs = agentenv::snapshot::repository::posixfs;
namespace repository = agentenv::snapshot::repository;

typedef posixfs::PosixFsSnapshotArtifactLayout Layout;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-posixfs-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }
};

SnapshotId FixtureId() {
    Uuid uuid;
    MT_EXPECT_TRUE(Uuid::Parse("01936f8e-72f5-7000-8000-000000000001", &uuid));
    return SnapshotId(uuid);
}

agentenv::snapshot::SnapshotAlias FixtureAlias(const std::string& value) {
    const agentenv::core::Expected<agentenv::snapshot::SnapshotAlias, std::string> alias =
        agentenv::snapshot::SnapshotAlias::Parse(value);
    MT_EXPECT_TRUE(alias.ok());
    return alias.value();
}

uint32_t ModeOf(const std::string& path) {
    struct stat info;
    MT_EXPECT_EQ(::stat(path.c_str(), &info), 0);
    return info.st_mode & 0777;
}

}  // namespace

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

MT_TEST(catalog_paths_nest_under_the_repository_root) {
    const std::string root = "/repo";
    MT_EXPECT_EQ(Layout::CatalogDir(root), std::string("/repo/catalog"));
    MT_EXPECT_EQ(Layout::AliasesDir(root), std::string("/repo/catalog/aliases"));
    MT_EXPECT_EQ(Layout::RecordsDir(root), std::string("/repo/catalog/records"));
    MT_EXPECT_EQ(Layout::SnapshotsDir(root), std::string("/repo/snapshots"));
    MT_EXPECT_EQ(Layout::ManagedLayersDir(root), std::string("/repo/managed-layers"));
    // Volumes live outside the snapshot catalog, not under it.
    MT_EXPECT_EQ(Layout::VolumesDir(root), std::string("/repo/volumes"));
    MT_EXPECT_EQ(Layout::VolumeAliasesDir(root), std::string("/repo/volumes/aliases"));
    MT_EXPECT_EQ(Layout::VolumeRecordsDir(root), std::string("/repo/volumes/records"));
}

MT_TEST(record_and_alias_paths_follow_the_rust_naming) {
    const std::string root = "/repo";
    const SnapshotId id = FixtureId();

    MT_EXPECT_EQ(Layout::RecordPath(root, id),
                 std::string("/repo/catalog/records/") + id.ToString() + ".json");
    // The alias file has no extension: the listing code filters records by
    // `.json`, which is what keeps aliases out of a record scan.
    MT_EXPECT_EQ(Layout::AliasPath(root, FixtureAlias("prod")),
                 std::string("/repo/catalog/aliases/prod"));
}

MT_TEST(lock_paths_sit_beside_what_they_guard) {
    const std::string root = "/repo";
    const SnapshotId id = FixtureId();

    // A `.lock` sibling, so the lock can be opened before the guarded file
    // exists — which is the normal case when creating a record.
    MT_EXPECT_EQ(Layout::RecordLockPath(root, id),
                 std::string("/repo/catalog/records/") + id.ToString() + ".lock");
    MT_EXPECT_EQ(Layout::AliasLockPath(root, FixtureAlias("prod")),
                 std::string("/repo/catalog/aliases/prod.lock"));
    MT_EXPECT_EQ(Layout::VolumeAliasLockPath(root, "data"),
                 std::string("/repo/volumes/aliases/data.lock"));
    MT_EXPECT_EQ(Layout::VolumeRecordLockPath(root, "vol1"),
                 std::string("/repo/volumes/records/vol1.lock"));

    // The lock must not collide with the record: `<id>.json` vs `<id>.lock`.
    MT_EXPECT_TRUE(Layout::RecordPath(root, id) != Layout::RecordLockPath(root, id));
}

MT_TEST(managed_layer_file_name_sanitizes_a_digest) {
    // A digest contains `:`, and a `/` would escape the managed-layers
    // directory entirely, so both are replaced rather than rejected.
    MT_EXPECT_EQ(posixfs::ManagedLayerFileName("sha256:abc123"),
                 std::string("sha256_abc123.overlaybd.commit"));
    MT_EXPECT_EQ(posixfs::ManagedLayerFileName("a/b:c"),
                 std::string("a_b_c.overlaybd.commit"));
    MT_EXPECT_EQ(Layout::ManagedLayerPath("/repo", "sha256:abc"),
                 std::string("/repo/managed-layers/sha256_abc.overlaybd.commit"));

    // The sanitised name stays a single path component.
    MT_EXPECT_TRUE(posixfs::ManagedLayerFileName("a/b").find('/') == std::string::npos);
}

MT_TEST(snapshot_dir_and_path_are_scoped_to_one_snapshot) {
    const SnapshotId id = FixtureId();
    const Layout layout("/repo", id);

    MT_EXPECT_EQ(layout.SnapshotDir(), std::string("/repo/snapshots/") + id.ToString());
    MT_EXPECT_EQ(layout.Path(posixfs::kPosixFsSnapshotCommitMarker),
                 std::string("/repo/snapshots/") + id.ToString() + "/commit");
    MT_EXPECT_EQ(layout.Path("rootfs/image.json"),
                 std::string("/repo/snapshots/") + id.ToString() + "/rootfs/image.json");
}

// ---------------------------------------------------------------------------
// RepositoryError
// ---------------------------------------------------------------------------

MT_TEST(repository_error_renders_every_variant) {
    MT_EXPECT_EQ(repository::RepositoryError::InvalidRequest("bad").ToString(),
                 std::string("invalid repository request: bad"));
    MT_EXPECT_EQ(repository::RepositoryError::SnapshotNotFound("x").ToString(),
                 std::string("snapshot not found: x"));
    MT_EXPECT_EQ(repository::RepositoryError::VolumeNotFound("v").ToString(),
                 std::string("volume not found: v"));
    MT_EXPECT_EQ(repository::RepositoryError::VolumeNameConflict("v").ToString(),
                 std::string("volume name already exists: v"));
    MT_EXPECT_EQ(repository::RepositoryError::AliasNotFound("a").ToString(),
                 std::string("snapshot alias not found: a"));
    MT_EXPECT_EQ(repository::RepositoryError::AliasConflict("a", "old", "new").ToString(),
                 std::string("alias 'a' already points to 'old', cannot rebind to 'new'"));
    MT_EXPECT_EQ(repository::RepositoryError::ArtifactNotFound("art").ToString(),
                 std::string("artifact not found: art"));
    MT_EXPECT_EQ(repository::RepositoryError::ManagedLayerNotFound("sha256:x").ToString(),
                 std::string("managed layer not found: sha256:x"));
    MT_EXPECT_EQ(
        repository::RepositoryError::IntegrityMismatch("art", "want", "got").ToString(),
        std::string("integrity mismatch for art: expected want, got got"));
    MT_EXPECT_EQ(repository::RepositoryError::Unsupported("f").ToString(),
                 std::string("unsupported operation: f"));
    MT_EXPECT_EQ(repository::RepositoryError::Backend("boom").ToString(),
                 std::string("backend error: boom"));
}

MT_TEST(repository_error_backend_folds_in_the_source) {
    // Rust keeps the cause as an `#[source]` chain; the port flattens it so
    // the underlying errno text is not lost.
    MT_EXPECT_EQ(repository::RepositoryError::Backend("write record", "No such file")
                     .ToString(),
                 std::string("backend error: write record: No such file"));
    // An empty source leaves the message untouched.
    MT_EXPECT_EQ(repository::RepositoryError::Backend("write record", "").ToString(),
                 std::string("backend error: write record"));
}

// ---------------------------------------------------------------------------
// WriteAtomic
// ---------------------------------------------------------------------------

MT_TEST(write_atomic_replaces_an_existing_file) {
    TempRoot root;
    const std::string target = root.Join("catalog/records/rec.json");

    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(target, "first").ok());
    MT_EXPECT_EQ(fs::ReadToString(target).value(), std::string("first"));

    // A second write replaces rather than appends.
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(target, "second").ok());
    MT_EXPECT_EQ(fs::ReadToString(target).value(), std::string("second"));
}

MT_TEST(write_atomic_creates_missing_parents) {
    TempRoot root;
    const std::string target = root.Join("a/b/c/file.json");
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(target, "{}").ok());
    MT_EXPECT_TRUE(fs::IsFile(target));
}

MT_TEST(write_atomic_leaves_no_temp_files_behind) {
    TempRoot root;
    const std::string dir = root.Join("records");
    MT_EXPECT_TRUE(fs::CreateDirAll(dir).ok());
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(fs::Join(dir, "rec.json"), "{}").ok());

    // The temp file is a sibling (rename is only atomic within a filesystem),
    // so a leak would show up right here.
    const std::vector<std::string> entries = fs::ReadDir(dir).value();
    MT_EXPECT_EQ(entries.size(), static_cast<std::size_t>(1));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        MT_EXPECT_TRUE(entries[i].find(".agentenv-tmp-") == std::string::npos);
    }
}

MT_TEST(write_atomic_produces_a_readable_file) {
    TempRoot root;
    const std::string target = root.Join("rec.json");
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(target, "{}").ok());
    // mkstemp creates 0600; catalog files must match the rest of the
    // repository instead.
    MT_EXPECT_EQ(ModeOf(target), static_cast<uint32_t>(0644));
}

MT_TEST(write_atomic_handles_empty_contents) {
    TempRoot root;
    const std::string target = root.Join("empty.json");
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(target, "").ok());
    MT_EXPECT_TRUE(fs::IsFile(target));
    MT_EXPECT_EQ(fs::ReadToString(target).value(), std::string(""));
}

// ---------------------------------------------------------------------------
// AcquireFileLock
// ---------------------------------------------------------------------------

MT_TEST(acquire_file_lock_creates_the_lock_and_its_directory) {
    TempRoot root;
    const std::string lock_path = root.Join("catalog/aliases/prod.lock");

    bool timed_out = false;
    const agentenv::core::Expected<FileLockGuard, std::string> guard =
        agentenv::core::AcquireFileLock(lock_path, "12345", 1000, &timed_out);
    MT_EXPECT_TRUE(guard.ok());
    MT_EXPECT_TRUE(!timed_out);
    MT_EXPECT_TRUE(fs::IsFile(lock_path));
    // The pid is written for diagnosis when a lock is stuck.
    MT_EXPECT_EQ(fs::ReadToString(lock_path).value(), std::string("12345"));
}

MT_TEST(acquire_file_lock_is_reentrant_after_release) {
    TempRoot root;
    const std::string lock_path = root.Join("a.lock");

    {
        const agentenv::core::Expected<FileLockGuard, std::string> first =
            agentenv::core::AcquireFileLock(lock_path, "1", 1000, NULL);
        MT_EXPECT_TRUE(first.ok());
        MT_EXPECT_TRUE(first.value().held());
    }
    // The guard released on scope exit, so this must succeed immediately.
    const agentenv::core::Expected<FileLockGuard, std::string> second =
        agentenv::core::AcquireFileLock(lock_path, "2", 1000, NULL);
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_EQ(fs::ReadToString(lock_path).value(), std::string("2"));
}

MT_TEST(acquire_file_lock_truncates_a_previous_owners_pid) {
    TempRoot root;
    const std::string lock_path = root.Join("a.lock");

    {
        MT_EXPECT_TRUE(
            agentenv::core::AcquireFileLock(lock_path, "1111111111", 1000, NULL).ok());
    }
    {
        MT_EXPECT_TRUE(agentenv::core::AcquireFileLock(lock_path, "22", 1000, NULL).ok());
    }
    // Without the truncate the shorter pid would leave the old digits behind.
    MT_EXPECT_EQ(fs::ReadToString(lock_path).value(), std::string("22"));
}

MT_TEST(acquire_file_lock_times_out_while_another_process_holds_it) {
    TempRoot root;
    const std::string lock_path = root.Join("a.lock");

    // flock is per open-file-description, so a second lock from the *same*
    // process would succeed; a child process is the only honest test.
    int ready_pipe[2];
    MT_EXPECT_EQ(::pipe(ready_pipe), 0);

    const pid_t pid = ::fork();
    MT_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        ::close(ready_pipe[0]);
        const agentenv::core::Expected<FileLockGuard, std::string> guard =
            agentenv::core::AcquireFileLock(lock_path, "child", 1000, NULL);
        const char signal_byte = guard.ok() ? 'y' : 'n';
        ssize_t ignored = ::write(ready_pipe[1], &signal_byte, 1);
        (void)ignored;
        ::usleep(400000);  // hold it past the parent's timeout
        ::_exit(0);
    }

    ::close(ready_pipe[1]);
    char signal_byte = 0;
    MT_EXPECT_EQ(::read(ready_pipe[0], &signal_byte, 1), static_cast<ssize_t>(1));
    ::close(ready_pipe[0]);
    MT_EXPECT_EQ(signal_byte, 'y');

    bool timed_out = false;
    const agentenv::core::Expected<FileLockGuard, std::string> blocked =
        agentenv::core::AcquireFileLock(lock_path, "parent", 100, &timed_out);
    MT_EXPECT_TRUE(!blocked.ok());
    // The caller reports a timeout differently from an I/O failure, so the
    // distinction has to survive.
    MT_EXPECT_TRUE(timed_out);
    MT_EXPECT_TRUE(blocked.error().find("timed out waiting for lock") != std::string::npos);

    int status = 0;
    ::waitpid(pid, &status, 0);

    // Once the child exits the lock is free again.
    MT_EXPECT_TRUE(agentenv::core::AcquireFileLock(lock_path, "parent", 2000, NULL).ok());
}

MT_TEST(acquire_file_lock_serializes_two_processes) {
    TempRoot root;
    const std::string lock_path = root.Join("a.lock");
    const std::string counter = root.Join("counter");
    MT_EXPECT_TRUE(agentenv::core::WriteAtomic(counter, "0").ok());

    // Each child does a lock / read / increment / write cycle; without the
    // lock the final count would be lower than the number of increments.
    const int kChildren = 4;
    const int kIterations = 20;
    std::vector<pid_t> pids;
    for (int c = 0; c < kChildren; ++c) {
        const pid_t pid = ::fork();
        MT_EXPECT_TRUE(pid >= 0);
        if (pid == 0) {
            for (int i = 0; i < kIterations; ++i) {
                const agentenv::core::Expected<FileLockGuard, std::string> guard =
                    agentenv::core::AcquireFileLock(lock_path, "c", 10000, NULL);
                if (!guard.ok()) ::_exit(1);
                const agentenv::core::Expected<std::string, std::string> current =
                    fs::ReadToString(counter);
                if (!current.ok()) ::_exit(1);
                const long value = std::strtol(current.value().c_str(), NULL, 10);
                char next[32];
                std::snprintf(next, sizeof(next), "%ld", value + 1);
                if (!agentenv::core::WriteAtomic(counter, next).ok()) ::_exit(1);
            }
            ::_exit(0);
        }
        pids.push_back(pid);
    }

    for (std::size_t i = 0; i < pids.size(); ++i) {
        int status = 0;
        ::waitpid(pids[i], &status, 0);
        MT_EXPECT_TRUE(WIFEXITED(status));
        MT_EXPECT_EQ(WEXITSTATUS(status), 0);
    }

    MT_EXPECT_EQ(fs::ReadToString(counter).value(),
                 std::string("80"));  // kChildren * kIterations
}

MT_TEST(file_lock_guard_moves_without_releasing) {
    TempRoot root;
    const std::string lock_path = root.Join("a.lock");

    agentenv::core::Expected<FileLockGuard, std::string> acquired =
        agentenv::core::AcquireFileLock(lock_path, "1", 1000, NULL);
    MT_EXPECT_TRUE(acquired.ok());

    FileLockGuard moved(std::move(acquired.value()));
    MT_EXPECT_TRUE(moved.held());
    // The moved-from guard must not release the descriptor the new owner
    // holds.
    MT_EXPECT_TRUE(!acquired.value().held());

    moved.Release();
    MT_EXPECT_TRUE(!moved.held());
    // Idempotent.
    moved.Release();
}

int main() { return microtest::RunAll(); }
