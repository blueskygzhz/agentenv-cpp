// SPDX-License-Identifier: MIT
// Rust: `std::fs` / `tokio::fs` — used pervasively by volume, setup, template.
//
// C++11 predates <filesystem>, so this is a thin POSIX layer with Rust-shaped
// semantics and error strings. Everything is synchronous: the Rust code is
// async only because it runs on tokio, not because these calls overlap.
#ifndef AGENTENV_CORE_FS_H_
#define AGENTENV_CORE_FS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace core {
namespace fs {

/// Rust `Path::exists`.
bool Exists(const std::string& path);
/// Rust `Path::is_dir`.
bool IsDir(const std::string& path);
/// Rust `Path::is_file`.
bool IsFile(const std::string& path);

/// Rust `Path::parent` — absent for "/" and for a bare file name.
Optional<std::string> Parent(const std::string& path);
/// Rust `Path::file_name`.
Optional<std::string> FileName(const std::string& path);
/// Rust `Path::extension` — without the dot, absent when there is none.
Optional<std::string> Extension(const std::string& path);
/// Rust `Path::join`, textual. An absolute `leaf` replaces `base` entirely.
std::string Join(const std::string& base, const std::string& leaf);

/// Rust `fs::create_dir_all`. Succeeds when the directory already exists.
Expected<Unit, std::string> CreateDirAll(const std::string& path);

/// Rust `fs::remove_dir_all`. Succeeds when the path is already absent, which
/// matches how callers use `let _ = remove_dir_all(..)` upstream.
Expected<Unit, std::string> RemoveDirAll(const std::string& path);

/// Rust `fs::remove_file`. Missing file is not an error, as above.
Expected<Unit, std::string> RemoveFile(const std::string& path);

/// Rust `fs::read` / `fs::read_to_string`.
Expected<std::string, std::string> ReadToString(const std::string& path);

/// Rust `fs::write` — truncating write, creating parents is *not* implied.
Expected<Unit, std::string> Write(const std::string& path, const std::string& contents);

/// Rust `File::create` + `set_len` — creates a sparse file of exactly `bytes`.
Expected<Unit, std::string> CreateSizedFile(const std::string& path, uint64_t bytes);

/// Rust `fs::hard_link`.
Expected<Unit, std::string> HardLink(const std::string& source, const std::string& destination);

/// Rust `fs::copy`.
Expected<Unit, std::string> Copy(const std::string& source, const std::string& destination);

/// Rust `fs::metadata(..).len()`.
Expected<uint64_t, std::string> FileSize(const std::string& path);

/// Rust `fs::read_dir` — entry names only (no "." / ".."), unsorted.
Expected<std::vector<std::string>, std::string> ReadDir(const std::string& path);

/// Rust `tempfile::tempdir()` — creates `{TMPDIR}/{prefix}XXXXXX`. The caller
/// owns removal; pair it with `RemoveDirAll`.
Expected<std::string, std::string> CreateTempDir(const std::string& prefix);

// ---------------------------------------------------------------------------
// Unix security primitives.
//
// These back the managed-secret handling, where the *exact* open flags,
// permission bits and ownership checks are the security property — not an
// implementation detail. Anything that merely reads or writes a file should
// use the plain helpers above instead.
// ---------------------------------------------------------------------------

/// Owning file descriptor. Secret handling checks metadata on an *already-open*
/// descriptor to avoid a path-based TOCTOU window, so descriptors outlive the
/// call that produced them and need real ownership.
///
/// Ownership is shared rather than unique: `Expected<T, E>` and `Optional<T>`
/// here are value-semantic containers that copy their payload, so a move-only
/// descriptor could not be returned through them. A reference-counted handle
/// keeps RAII (the fd closes exactly once, when the last copy dies) while
/// staying copyable.
class FileDescriptor {
 public:
    FileDescriptor() {}
    /// Takes ownership of `fd`. A negative value yields an empty handle.
    explicit FileDescriptor(int fd);

    bool valid() const { return holder_ && *holder_ >= 0; }
    int get() const { return holder_ ? *holder_ : -1; }

    /// Drops this reference; closes the descriptor if it was the last one.
    void reset() { holder_.reset(); }

 private:
    std::shared_ptr<int> holder_;
};

/// The subset of `stat(2)` the security checks need. Mirrors what Rust reads
/// through `MetadataExt` / `PermissionsExt`.
struct FileStat {
    bool is_symlink = false;
    bool is_dir = false;
    bool is_regular = false;
    /// Permission and type bits, already masked to 0o7777.
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t size = 0;
};

/// Rust `fs::symlink_metadata` — does *not* follow the final symlink, which is
/// what lets a caller reject one.
Expected<FileStat, std::string> SymlinkStat(const std::string& path);

/// Rust `fs::metadata` — follows symlinks.
Expected<FileStat, std::string> Stat(const std::string& path);

/// `fstat` on an open descriptor. Preferred over `Stat` once a file is open,
/// since it describes the object actually being read.
Expected<FileStat, std::string> StatFd(int fd);

/// Opens for reading with `O_NONBLOCK`, following symlinks. Rust
/// `ApiKey::open_external`: a Kubernetes projected secret is a symlink, so the
/// external path must stay followable.
Expected<FileDescriptor, std::string> OpenReadFollow(const std::string& path);

/// Opens for reading with `O_NOFOLLOW | O_NONBLOCK`. Rust
/// `managed_secret::open_secret`: our own secret must never be a symlink, so
/// the kernel rejects one for us (ELOOP).
Expected<FileDescriptor, std::string> OpenReadNoFollow(const std::string& path);

/// Like `OpenReadNoFollow`, but reports a missing file as an empty optional
/// rather than an error — Rust's `error.kind() == NotFound` branch.
///
/// This exists because `errno` cannot be inspected reliably by the caller: any
/// intervening library call may overwrite it. Classifying the failure here, on
/// the statement right after `open`, is the only sound place to do it.
Expected<Optional<FileDescriptor>, std::string> OpenReadNoFollowOptional(
    const std::string& path);

/// Same NotFound-tolerant variant for the symlink-following open, used for
/// externally provided (e.g. Kubernetes projected) secrets.
Expected<Optional<FileDescriptor>, std::string> OpenReadFollowOptional(const std::string& path);

/// Reads at most `limit` bytes from `fd`. Rust `Read::take(limit)`.
Expected<std::string, std::string> ReadFdToString(int fd, std::size_t limit);

/// Writes all of `contents` to an already-open `fd`. Writing through the
/// descriptor (rather than reopening by path) is what makes a subsequent
/// `SyncFd` actually durable for these bytes, and closes a TOCTOU window.
Expected<Unit, std::string> WriteFd(int fd, const std::string& contents);

/// Rust `fs::set_permissions`.
Expected<Unit, std::string> SetPermissions(const std::string& path, uint32_t mode);

/// Rust `fs::DirBuilder::mode(m).recursive(true).create(path)` — every
/// component created gets `mode`, unmodified by the umask.
Expected<Unit, std::string> CreateDirAllWithMode(const std::string& path, uint32_t mode);

/// `fsync` the directory at `path`, so a rename into it is durable. Rust
/// `File::open(parent).and_then(sync_all)`.
Expected<Unit, std::string> SyncDirectory(const std::string& path);

/// `fsync` an open descriptor. Rust `File::sync_all`.
Expected<Unit, std::string> SyncFd(int fd);

/// Rust `NamedTempFile::persist_noclobber` — atomically publishes `source` as
/// `destination`, never overwriting. Returns false when `destination` already
/// exists, which callers treat as "someone else won the race", not an error.
/// Implemented with `link(2)` + `unlink(2)` because `rename(2)` would clobber.
Expected<bool, std::string> PersistNoClobber(const std::string& source,
                                             const std::string& destination);

/// Rust `NamedTempFile::new_in` — creates `{dir}/{prefix}XXXXXX` and hands back
/// both the open descriptor and its path.
struct TempFile {
    FileDescriptor fd;
    std::string path;
};
Expected<TempFile, std::string> CreateTempFileIn(const std::string& dir,
                                                 const std::string& prefix);

/// Rust `nix::unistd::Uid::effective().as_raw()`.
uint32_t EffectiveUid();

/// Rust `Uid::effective().is_root()`.
bool EffectiveUidIsRoot();

}  // namespace fs
}  // namespace core
}  // namespace agentenv
#endif  // AGENTENV_CORE_FS_H_
