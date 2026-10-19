// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/cached_fs.rs
#include "agentenv/storage/overlaybd/cached_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>

#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/sys.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

std::string Errno(const char* what, const std::string& path) {
    std::ostringstream o;
    o << what << " " << path << ": " << ::strerror(errno);
    return o.str();
}

/// Rust's `bail!("<op> is not supported for this cached source")`.
std::string Unsupported(const char* op) {
    std::ostringstream o;
    o << op << " is not supported for this cached source";
    return o.str();
}

/// Rust's `bail!("<op> is not supported without source fs")`.
std::string NoSource(const char* op) {
    std::ostringstream o;
    o << op << " is not supported without source fs";
    return o.str();
}

/// `mkdir -p` over everything but the final component.
bool MakeParents(const std::string& path) {
    const std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string parent = path.substr(0, slash);

    std::string built;
    std::string::size_type i = 0;
    while (i <= parent.size()) {
        const std::string::size_type next = parent.find('/', i);
        const std::string::size_type end = next == std::string::npos ? parent.size() : next;
        built = parent.substr(0, end);
        if (!built.empty() && ::mkdir(built.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// CachedFsSource — defaults
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> CachedFsSource::Mkdir(const std::string&, uint32_t) {
    return core::make_unexpected(Unsupported("mkdir"));
}

core::Expected<core::Unit, std::string> CachedFsSource::Rmdir(const std::string&) {
    return core::make_unexpected(Unsupported("rmdir"));
}

core::Expected<std::string, std::string> CachedFsSource::Readlink(const std::string&) {
    return core::make_unexpected(Unsupported("readlink"));
}

core::Expected<struct stat, std::string> CachedFsSource::Stat(const std::string&) {
    return core::make_unexpected(Unsupported("stat"));
}

core::Expected<struct stat, std::string> CachedFsSource::Lstat(const std::string&) {
    return core::make_unexpected(Unsupported("lstat"));
}

core::Expected<struct statvfs, std::string> CachedFsSource::Statvfs(const std::string&) {
    return core::make_unexpected(Unsupported("statvfs"));
}

core::Expected<std::vector<std::string>, std::string> CachedFsSource::Opendir(
    const std::string&) {
    return core::make_unexpected(Unsupported("opendir"));
}

core::Expected<core::Unit, std::string> CachedFsSource::Unlink(const std::string&) {
    return core::make_unexpected(Unsupported("unlink"));
}

core::Expected<core::Unit, std::string> CachedFsSource::Access(const std::string&, int) {
    return core::make_unexpected(Unsupported("access"));
}

core::Expected<std::vector<uint8_t>, std::string> CachedFsSource::GetXattr(
    const std::string&, const std::string&) {
    return core::make_unexpected(Unsupported("getxattr"));
}

core::Expected<std::vector<std::string>, std::string> CachedFsSource::ListXattr(
    const std::string&) {
    return core::make_unexpected(Unsupported("listxattr"));
}

core::Expected<core::Unit, std::string> CachedFsSource::SetXattr(const std::string&,
                                                                    const std::string&,
                                                                    const uint8_t*, size_t,
                                                                    int) {
    return core::make_unexpected(Unsupported("setxattr"));
}

core::Expected<core::Unit, std::string> CachedFsSource::RemoveXattr(const std::string&,
                                                                       const std::string&) {
    return core::make_unexpected(Unsupported("removexattr"));
}

// ---------------------------------------------------------------------------
// LocalFsSource
// ---------------------------------------------------------------------------

std::string LocalFsSource::ResolvePath(const std::string& pathname) const {
    // The leading slash is stripped rather than honoured: an absolute-looking
    // name is still relative to the root, and joining it as-is would escape.
    size_t i = 0;
    while (i < pathname.size() && pathname[i] == '/') ++i;
    const std::string relative = pathname.substr(i);
    if (relative.empty()) return root_;
    if (!root_.empty() && root_[root_.size() - 1] == '/') return root_ + relative;
    return root_ + "/" + relative;
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> LocalFsSource::Open(
    const std::string& pathname, uint32_t flags, uint32_t mode) {
    const std::string path = ResolvePath(pathname);
    const int oflags = static_cast<int>(flags);
    const int accmode = oflags & O_ACCMODE;

    bool read = accmode != O_WRONLY;
    bool write = accmode == O_WRONLY || accmode == O_RDWR;
    if (!read && !write) {
        // Rust: "Keep sane default if caller passes cache control flags only."
        // The cache-control bits live outside O_ACCMODE, so a caller passing
        // only those leaves the access mode at zero -- which is O_RDONLY, but
        // reached by accident rather than by request.
        read = true;
        write = false;
    }

    const bool create = (oflags & O_CREAT) != 0;
    const bool truncate = (oflags & O_TRUNC) != 0;
    if (create && !MakeParents(path)) {
        return core::make_unexpected(Errno("create parent of", path));
    }

    LocalFileBuilder builder;
    builder.Read(read).Write(write).Create(create).Truncate(truncate).Mode(mode);
    const core::Expected<std::shared_ptr<LocalFile>, std::string> file = builder.Open(path);
    if (!file.ok()) return core::make_unexpected(file.error());
    return std::shared_ptr<VirtualFile>(file.value());
}

core::Expected<core::Unit, std::string> LocalFsSource::Mkdir(const std::string& pathname,
                                                                uint32_t mode) {
    const std::string path = ResolvePath(pathname);
    if (::mkdir(path.c_str(), 0777) != 0) {
        return core::make_unexpected(Errno("mkdir", path));
    }
    // Set separately, as Rust does: `mkdir(2)` applies the umask to its mode
    // argument, so the requested bits would not survive it.
    if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) {
        return core::make_unexpected(Errno("chmod", path));
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LocalFsSource::Rmdir(const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    if (::rmdir(path.c_str()) != 0) return core::make_unexpected(Errno("rmdir", path));
    return core::Unit();
}

core::Expected<std::string, std::string> LocalFsSource::Readlink(
    const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    // Grown until it fits: a link's length can change between the stat and
    // the read, so a size taken up front is only a hint.
    std::vector<char> buf(256, 0);
    for (;;) {
        const ssize_t n = ::readlink(path.c_str(), &buf[0], buf.size());
        if (n < 0) return core::make_unexpected(Errno("readlink", path));
        if (static_cast<size_t>(n) < buf.size()) return std::string(&buf[0], n);
        if (buf.size() >= (1u << 20)) {
            return core::make_unexpected("readlink " + path + ": target too long");
        }
        buf.resize(buf.size() * 2);
    }
}

core::Expected<struct stat, std::string> LocalFsSource::Stat(const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return core::make_unexpected(Errno("stat", path));
    return st;
}

core::Expected<struct stat, std::string> LocalFsSource::Lstat(const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return core::make_unexpected(Errno("lstat", path));
    return st;
}

core::Expected<struct statvfs, std::string> LocalFsSource::Statvfs(
    const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    struct statvfs st;
    if (::statvfs(path.c_str(), &st) != 0) {
        return core::make_unexpected(Errno("statvfs", path));
    }
    return st;
}

core::Expected<std::vector<std::string>, std::string> LocalFsSource::Opendir(
    const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    DIR* dir = ::opendir(path.c_str());
    if (dir == 0) return core::make_unexpected(Errno("opendir", path));

    std::vector<std::string> out;
    for (;;) {
        errno = 0;
        const struct dirent* ent = ::readdir(dir);
        if (ent == 0) {
            if (errno != 0) {
                const std::string err = Errno("readdir", path);
                ::closedir(dir);
                return core::make_unexpected(err);
            }
            break;
        }
        // `.` and `..` are kept: Rust's `read_dir` does not yield them, and
        // neither does this -- `readdir(3)` does, so they are filtered here
        // to match.
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(dir);
    // Sorted, as Rust does with `sort_unstable`: directory order is
    // filesystem-defined, and a caller comparing two listings needs them
    // stable.
    std::sort(out.begin(), out.end());
    return out;
}

core::Expected<core::Unit, std::string> LocalFsSource::Unlink(const std::string& pathname) {
    const std::string path = ResolvePath(pathname);
    if (::unlink(path.c_str()) != 0) return core::make_unexpected(Errno("unlink", path));
    return core::Unit();
}

core::Expected<core::Unit, std::string> LocalFsSource::Access(const std::string& pathname,
                                                                 int mode) {
    const std::string path = ResolvePath(pathname);
    if (::access(path.c_str(), mode) != 0) return core::make_unexpected(Errno("access", path));
    return core::Unit();
}

core::Expected<std::vector<uint8_t>, std::string> LocalFsSource::GetXattr(
    const std::string& path, const std::string& name) {
    const std::string full = ResolvePath(path);
    const core::Expected<std::vector<uint8_t>, sys::SysError> got = sys::GetXattr(full, name);
    if (!got.ok()) {
        return core::make_unexpected("getxattr " + name + " on " + full + ": " +
                                     got.error().Message());
    }
    return got.value();
}

core::Expected<std::vector<std::string>, std::string> LocalFsSource::ListXattr(
    const std::string& path) {
    const std::string full = ResolvePath(path);
    const core::Expected<std::vector<std::string>, sys::SysError> got = sys::ListXattr(full);
    if (!got.ok()) {
        return core::make_unexpected("listxattr on " + full + ": " + got.error().Message());
    }
    return got.value();
}

core::Expected<core::Unit, std::string> LocalFsSource::SetXattr(const std::string& path,
                                                                   const std::string& name,
                                                                   const uint8_t* value,
                                                                   size_t len, int flags) {
    const std::string full = ResolvePath(path);
    const core::Expected<core::Unit, sys::SysError> done =
        sys::SetXattr(full, name, value, len, flags);
    if (!done.ok()) {
        return core::make_unexpected("setxattr " + name + " on " + full + ": " +
                                     done.error().Message());
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> LocalFsSource::RemoveXattr(const std::string& path,
                                                                      const std::string& name) {
    const std::string full = ResolvePath(path);
    const core::Expected<core::Unit, sys::SysError> done = sys::RemoveXattr(full, name);
    if (!done.ok()) {
        return core::make_unexpected("removexattr " + name + " on " + full + ": " +
                                     done.error().Message());
    }
    return core::Unit();
}

// ---------------------------------------------------------------------------
// LazySourceFile
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<VirtualFile>, std::string> LazySourceFile::OpenIfNeeded() {
    if (file_) return file_;
    if (!source_) return core::make_unexpected(std::string("lazy source file has no source"));

    // Always read-only. Rust states it outright ("NOTE: source file is read
    // only") and it is what makes the laziness safe: a write would have to
    // decide whether it lands in the cache or the source, and the cache is
    // the only writer of record.
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> opened =
        source_->Open(pathname_, static_cast<uint32_t>(O_RDONLY), mode_);
    if (!opened.ok()) return opened;
    file_ = opened.value();
    return file_;
}

core::Expected<ByteBuffer, std::string> LazySourceFile::ReadAt(uint64_t offset, size_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> file = OpenIfNeeded();
    if (!file.ok()) return core::make_unexpected(file.error());
    return file.value()->ReadAt(offset, len);
}

core::Expected<size_t, std::string> LazySourceFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                                 size_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> file = OpenIfNeeded();
    if (!file.ok()) return core::make_unexpected(file.error());
    return file.value()->ReadAtInto(offset, dst, len);
}

core::Expected<size_t, std::string> LazySourceFile::WriteAt(uint64_t offset,
                                                              const uint8_t* data,
                                                              size_t len) {
    // Delegated rather than refused here. Rust forwards it too: the handle was
    // opened read-only, so the refusal comes from the underlying file and says
    // so, instead of this layer inventing a different error.
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> file = OpenIfNeeded();
    if (!file.ok()) return core::make_unexpected(file.error());
    return file.value()->WriteAt(offset, data, len);
}

core::Expected<uint64_t, std::string> LazySourceFile::Size() {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> file = OpenIfNeeded();
    if (!file.ok()) return core::make_unexpected(file.error());
    return file.value()->Size();
}

// ---------------------------------------------------------------------------
// CachedFs
// ---------------------------------------------------------------------------

core::Expected<std::shared_ptr<CachedFile>, std::string> CachedFs::OpenWithMode(
    const std::string& pathname, uint32_t flags, uint32_t mode) {
    if (!pool_) return core::make_unexpected(std::string("cached fs has no cache pool"));

    // Only the cache-control bits are forwarded. The access mode and creation
    // flags describe how to open the *source*, and the pool has no use for
    // them -- it is told the source separately.
    const uint32_t cflags = flags & kOCacheOnly;

    if (source_) {
        // Rust substitutes 0o644 for a zero mode: the mode only matters if
        // the source has to create the file, and a zero mode would create it
        // unreadable.
        const uint32_t source_mode = mode == 0 ? 0644 : mode;
        std::shared_ptr<VirtualFile> source(new LazySourceFile(source_, pathname,
                                                               source_mode));
        // Size 0 means "ask the source", which the lazy handle answers by
        // opening on demand -- so a cache hit still never opens it.
        return pool_->OpenFile(pathname, source, 0, cflags);
    }

    // No source: the size has to come from the existing entry, because there
    // is nothing to probe. Zero when absent, which `OpenFile` then refuses --
    // a cache-only open of something never cached cannot be satisfied.
    uint64_t initial_size = 0;
    pool_->CachedSizeBySrcName(pathname, &initial_size);
    // `kOCacheOnly` is forced on, not merely passed through: without a source
    // a refill is impossible, so a read that tried would fail obscurely
    // instead of reporting a miss.
    return pool_->OpenFile(pathname, std::shared_ptr<VirtualFile>(), initial_size,
                           cflags | kOCacheOnly);
}

core::Expected<std::shared_ptr<CachedFile>, std::string> CachedFs::Open(
    const std::string& pathname, uint32_t flags) {
    return OpenWithMode(pathname, flags, 0);
}

core::Expected<core::Unit, std::string> CachedFs::Rename(const std::string& oldname,
                                                            const std::string& newname) {
    if (!pool_) return core::make_unexpected(std::string("cached fs has no cache pool"));
    // The cache entry only. Rust does the same: the source is not renamed,
    // because a cached filesystem may be backed by a read-only source where
    // renaming is not available at all.
    return pool_->RenameSrcName(oldname, newname);
}

core::Expected<core::Unit, std::string> CachedFs::Unlink(const std::string& pathname) {
    if (!pool_) return core::make_unexpected(std::string("cached fs has no cache pool"));

    const core::Expected<core::Unit, std::string> evicted = pool_->EvictSrcName(pathname);
    if (source_) {
        // The source's status wins, as Rust notes for C++ parity. The source
        // holds the file; a stale cache entry for something successfully
        // removed is not worth failing the call over.
        return source_->Unlink(pathname);
    }
    return evicted;
}

core::Expected<core::Unit, std::string> CachedFs::Access(const std::string& pathname,
                                                            int mode) {
    if (source_) return source_->Access(pathname, mode);
    if (!pool_) return core::make_unexpected(std::string("cached fs has no cache pool"));
    // Answered from the cache so a source-less filesystem can still say what
    // it holds. The mode is ignored here on purpose: the cache has no
    // permissions of its own to check against.
    if (pool_->ContainsSrcName(pathname)) return core::Unit();
    return core::make_unexpected("cache entry not found for " + pathname);
}

core::Expected<core::Unit, std::string> CachedFs::Mkdir(const std::string& pathname,
                                                           uint32_t mode) {
    if (!source_) return core::make_unexpected(NoSource("mkdir"));
    return source_->Mkdir(pathname, mode);
}

core::Expected<core::Unit, std::string> CachedFs::Rmdir(const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("rmdir"));
    return source_->Rmdir(pathname);
}

core::Expected<std::string, std::string> CachedFs::Readlink(const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("readlink"));
    return source_->Readlink(pathname);
}

core::Expected<struct stat, std::string> CachedFs::Stat(const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("stat"));
    return source_->Stat(pathname);
}

core::Expected<struct stat, std::string> CachedFs::Lstat(const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("lstat"));
    return source_->Lstat(pathname);
}

core::Expected<struct statvfs, std::string> CachedFs::Statvfs(const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("statvfs"));
    return source_->Statvfs(pathname);
}

core::Expected<std::vector<std::string>, std::string> CachedFs::Opendir(
    const std::string& pathname) {
    if (!source_) return core::make_unexpected(NoSource("opendir"));
    return source_->Opendir(pathname);
}

core::Expected<std::vector<uint8_t>, std::string> CachedFs::GetXattr(
    const std::string& path, const std::string& name) {
    if (!source_) return core::make_unexpected(NoSource("getxattr"));
    return source_->GetXattr(path, name);
}

core::Expected<std::vector<std::string>, std::string> CachedFs::ListXattr(
    const std::string& path) {
    if (!source_) return core::make_unexpected(NoSource("listxattr"));
    return source_->ListXattr(path);
}

core::Expected<core::Unit, std::string> CachedFs::SetXattr(const std::string& path,
                                                              const std::string& name,
                                                              const uint8_t* value,
                                                              size_t len, int flags) {
    if (!source_) return core::make_unexpected(NoSource("setxattr"));
    return source_->SetXattr(path, name, value, len, flags);
}

core::Expected<core::Unit, std::string> CachedFs::RemoveXattr(const std::string& path,
                                                                 const std::string& name) {
    if (!source_) return core::make_unexpected(NoSource("removexattr"));
    return source_->RemoveXattr(path, name);
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
