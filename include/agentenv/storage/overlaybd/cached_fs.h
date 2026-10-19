// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/cached_fs.rs — a filesystem facade
// that serves reads from the block cache and falls through to a source.
//
// The split is the point: the cache pool below knows about blocks and
// eviction but not about paths, while callers want to open a name. This layer
// maps one onto the other, and nothing else.
#ifndef AGENTENV_STORAGE_OVERLAYBD_CACHED_FS_H_
#define AGENTENV_STORAGE_OVERLAYBD_CACHED_FS_H_

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/cache_pool.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `CachedFsSource` — where a cache miss goes.
///
/// Every method but `Open` defaults to refusing. Rust spells these as
/// `bail!("... is not supported for this cached source")`, and the default
/// matters: a remote source has no directories to create and no xattrs to
/// list, so a source that implements only `Open` is the normal case rather
/// than an incomplete one.
class CachedFsSource {
 public:
    virtual ~CachedFsSource() {}

    /// The only required operation.
    virtual core::Expected<std::shared_ptr<VirtualFile>, std::string> Open(
        const std::string& pathname, uint32_t flags, uint32_t mode) = 0;

    virtual core::Expected<core::Unit, std::string> Mkdir(const std::string& pathname,
                                                             uint32_t mode);
    virtual core::Expected<core::Unit, std::string> Rmdir(const std::string& pathname);
    virtual core::Expected<std::string, std::string> Readlink(const std::string& pathname);
    virtual core::Expected<struct stat, std::string> Stat(const std::string& pathname);
    virtual core::Expected<struct stat, std::string> Lstat(const std::string& pathname);
    virtual core::Expected<struct statvfs, std::string> Statvfs(const std::string& pathname);
    virtual core::Expected<std::vector<std::string>, std::string> Opendir(
        const std::string& pathname);
    virtual core::Expected<core::Unit, std::string> Unlink(const std::string& pathname);
    virtual core::Expected<core::Unit, std::string> Access(const std::string& pathname,
                                                              int mode);

    virtual core::Expected<std::vector<uint8_t>, std::string> GetXattr(
        const std::string& path, const std::string& name);
    virtual core::Expected<std::vector<std::string>, std::string> ListXattr(
        const std::string& path);
    virtual core::Expected<core::Unit, std::string> SetXattr(const std::string& path,
                                                                const std::string& name,
                                                                const uint8_t* value,
                                                                size_t len, int flags);
    virtual core::Expected<core::Unit, std::string> RemoveXattr(const std::string& path,
                                                                   const std::string& name);
};

/// Rust `LocalFsSource` — a source rooted at a local directory.
class LocalFsSource : public CachedFsSource {
 public:
    explicit LocalFsSource(const std::string& root) : root_(root) {}

    core::Expected<std::shared_ptr<VirtualFile>, std::string> Open(
        const std::string& pathname, uint32_t flags, uint32_t mode);

    core::Expected<core::Unit, std::string> Mkdir(const std::string& pathname, uint32_t mode);
    core::Expected<core::Unit, std::string> Rmdir(const std::string& pathname);
    core::Expected<std::string, std::string> Readlink(const std::string& pathname);
    core::Expected<struct stat, std::string> Stat(const std::string& pathname);
    core::Expected<struct stat, std::string> Lstat(const std::string& pathname);
    core::Expected<struct statvfs, std::string> Statvfs(const std::string& pathname);
    core::Expected<std::vector<std::string>, std::string> Opendir(const std::string& pathname);
    core::Expected<core::Unit, std::string> Unlink(const std::string& pathname);
    core::Expected<core::Unit, std::string> Access(const std::string& pathname, int mode);

    core::Expected<std::vector<uint8_t>, std::string> GetXattr(const std::string& path,
                                                                  const std::string& name);
    core::Expected<std::vector<std::string>, std::string> ListXattr(const std::string& path);
    core::Expected<core::Unit, std::string> SetXattr(const std::string& path,
                                                        const std::string& name,
                                                        const uint8_t* value, size_t len,
                                                        int flags);
    core::Expected<core::Unit, std::string> RemoveXattr(const std::string& path,
                                                           const std::string& name);

    /// Rust `resolve_path` — `<root>/<pathname>`, with the leading slash
    /// stripped so an absolute-looking name stays inside the root.
    std::string ResolvePath(const std::string& pathname) const;

 private:
    std::string root_;
};

/// Rust `LazySourceFile` — a source file opened on first use.
///
/// The laziness is what makes a cache hit free: opening a cached file must
/// not touch the source, and a path that is fully cached may never open it at
/// all. For a remote source that saves a request per file.
class LazySourceFile : public VirtualFile {
 public:
    LazySourceFile(std::shared_ptr<CachedFsSource> source, const std::string& pathname,
                   uint32_t mode)
        : source_(source), pathname_(pathname), mode_(mode) {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len);
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst, size_t len);
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                   size_t len);
    core::Expected<uint64_t, std::string> Size();

 private:
    core::Expected<std::shared_ptr<VirtualFile>, std::string> OpenIfNeeded();

    std::shared_ptr<CachedFsSource> source_;
    std::string pathname_;
    uint32_t mode_;
    std::shared_ptr<VirtualFile> file_;
};

/// Rust `CachedFs`.
class CachedFs {
 public:
    CachedFs(std::shared_ptr<CachedFsSource> source,
             std::shared_ptr<FileCacheBackend> pool)
        : source_(source), pool_(pool) {}

    /// Rust `open_with_mode` / `open`.
    ///
    /// With a source, the file is backed by it and misses refill. Without
    /// one, the open is cache-only: it can only serve what is already there,
    /// and its size comes from the existing entry because there is nothing to
    /// ask.
    core::Expected<std::shared_ptr<CachedFile>, std::string> OpenWithMode(
        const std::string& pathname, uint32_t flags, uint32_t mode);
    core::Expected<std::shared_ptr<CachedFile>, std::string> Open(const std::string& pathname,
                                                                     uint32_t flags);

    /// Rust `rename` — renames the *cache entry*, not the source.
    core::Expected<core::Unit, std::string> Rename(const std::string& oldname,
                                                      const std::string& newname);

    /// Rust `unlink`.
    ///
    /// Evicts the cache entry first, then unlinks in the source. When a
    /// source exists its status is what the caller sees, because the source
    /// is the thing that actually holds the file -- an eviction failure for a
    /// file that is then successfully removed is not an error worth
    /// surfacing.
    core::Expected<core::Unit, std::string> Unlink(const std::string& pathname);

    /// Rust `access`.
    ///
    /// Without a source this answers from the cache: an entry exists or it
    /// does not. That makes a cache-only filesystem still able to say what it
    /// holds.
    core::Expected<core::Unit, std::string> Access(const std::string& pathname, int mode);

    /// Rust's metadata passthroughs. Each refuses without a source: the cache
    /// stores bytes, and has no directories, links or permissions to report.
    core::Expected<core::Unit, std::string> Mkdir(const std::string& pathname, uint32_t mode);
    core::Expected<core::Unit, std::string> Rmdir(const std::string& pathname);
    core::Expected<std::string, std::string> Readlink(const std::string& pathname);
    core::Expected<struct stat, std::string> Stat(const std::string& pathname);
    core::Expected<struct stat, std::string> Lstat(const std::string& pathname);
    core::Expected<struct statvfs, std::string> Statvfs(const std::string& pathname);
    core::Expected<std::vector<std::string>, std::string> Opendir(const std::string& pathname);

    core::Expected<std::vector<uint8_t>, std::string> GetXattr(const std::string& path,
                                                                  const std::string& name);
    core::Expected<std::vector<std::string>, std::string> ListXattr(const std::string& path);
    core::Expected<core::Unit, std::string> SetXattr(const std::string& path,
                                                        const std::string& name,
                                                        const uint8_t* value, size_t len,
                                                        int flags);
    core::Expected<core::Unit, std::string> RemoveXattr(const std::string& path,
                                                           const std::string& name);

    /// Rust `get_source` / `set_source`.
    ///
    /// Swappable at runtime: a filesystem can start cache-only and gain a
    /// source once one is reachable, which is how a lazily-attached remote
    /// backend is wired in.
    std::shared_ptr<CachedFsSource> GetSource() const { return source_; }
    void SetSource(std::shared_ptr<CachedFsSource> source) { source_ = source; }

 private:
    CachedFs(const CachedFs&);
    CachedFs& operator=(const CachedFs&);

    std::shared_ptr<CachedFsSource> source_;
    std::shared_ptr<FileCacheBackend> pool_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_CACHED_FS_H_
