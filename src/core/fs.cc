// SPDX-License-Identifier: MIT
// Rust: `std::fs` / `tokio::fs`
#include "agentenv/core/fs.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace agentenv {
namespace core {
namespace fs {
namespace {

std::string Errno(const char* op, const std::string& path, int err) {
    std::ostringstream oss;
    oss << op << " '" << path << "': " << std::strerror(err);
    return oss.str();
}

bool Stat(const std::string& path, struct stat* out) {
    return ::stat(path.c_str(), out) == 0;
}

}  // namespace

bool Exists(const std::string& path) {
    struct stat st;
    return !path.empty() && Stat(path, &st);
}

bool IsDir(const std::string& path) {
    struct stat st;
    return !path.empty() && Stat(path, &st) && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat st;
    return !path.empty() && Stat(path, &st) && S_ISREG(st.st_mode);
}

Optional<std::string> Parent(const std::string& path) {
    if (path.empty() || path == "/") return Optional<std::string>();
    // Ignore a trailing slash, as Rust's `Path::parent` does.
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    const std::size_t slash = path.rfind('/', end - 1);
    if (slash == std::string::npos) return Optional<std::string>();
    if (slash == 0) return Optional<std::string>(std::string("/"));
    return Optional<std::string>(path.substr(0, slash));
}

Optional<std::string> FileName(const std::string& path) {
    if (path.empty() || path == "/") return Optional<std::string>();
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    const std::size_t slash = path.rfind('/', end - 1);
    const std::size_t begin = slash == std::string::npos ? 0 : slash + 1;
    if (begin >= end) return Optional<std::string>();
    const std::string name = path.substr(begin, end - begin);
    if (name == "." || name == "..") return Optional<std::string>();
    return Optional<std::string>(name);
}

Optional<std::string> Extension(const std::string& path) {
    const Optional<std::string> name = FileName(path);
    if (!name.has_value()) return Optional<std::string>();
    const std::size_t dot = name->rfind('.');
    // A leading dot means a hidden file, not an extension (Rust semantics).
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name->size()) {
        return Optional<std::string>();
    }
    return Optional<std::string>(name->substr(dot + 1));
}

std::string Join(const std::string& base, const std::string& leaf) {
    if (!leaf.empty() && leaf[0] == '/') return leaf;
    if (base.empty()) return leaf;
    if (leaf.empty()) return base;
    if (base[base.size() - 1] == '/') return base + leaf;
    return base + "/" + leaf;
}

Expected<Unit, std::string> CreateDirAll(const std::string& path) {
    if (path.empty()) return make_unexpected(std::string("create_dir_all '': empty path"));
    if (IsDir(path)) return Unit();

    std::string progress;
    std::size_t cursor = 0;
    if (path[0] == '/') {
        progress = "/";
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string component =
            path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!component.empty()) {
            progress = Join(progress, component);
            if (::mkdir(progress.c_str(), 0755) != 0 && errno != EEXIST) {
                return make_unexpected(Errno("create_dir_all", progress, errno));
            }
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }
    return Unit();
}

Expected<Unit, std::string> RemoveDirAll(const std::string& path) {
    struct stat st;
    if (path.empty() || ::lstat(path.c_str(), &st) != 0) {
        return Unit();  // already gone
    }
    if (!S_ISDIR(st.st_mode)) {
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            return make_unexpected(Errno("remove_dir_all", path, errno));
        }
        return Unit();
    }

    DIR* dir = ::opendir(path.c_str());
    if (dir == NULL) return make_unexpected(Errno("remove_dir_all", path, errno));
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const Expected<Unit, std::string> removed = RemoveDirAll(Join(path, name));
        if (!removed.ok()) {
            ::closedir(dir);
            return removed;
        }
    }
    ::closedir(dir);

    if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
        return make_unexpected(Errno("remove_dir_all", path, errno));
    }
    return Unit();
}

Expected<Unit, std::string> RemoveFile(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return make_unexpected(Errno("remove_file", path, errno));
    }
    return Unit();
}

Expected<std::string, std::string> ReadToString(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return make_unexpected(Errno("read", path, errno ? errno : ENOENT));
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) return make_unexpected(Errno("read", path, EIO));
    return buffer.str();
}

Expected<Unit, std::string> Write(const std::string& path, const std::string& contents) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) return make_unexpected(Errno("write", path, errno ? errno : EACCES));
    if (!contents.empty()) {
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    output.flush();
    if (!output) return make_unexpected(Errno("write", path, EIO));
    return Unit();
}

Expected<Unit, std::string> CreateSizedFile(const std::string& path, uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return make_unexpected(Errno("create", path, errno));
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        const int saved = errno;
        ::close(fd);
        return make_unexpected(Errno("set_len", path, saved));
    }
    ::close(fd);
    return Unit();
}

Expected<Unit, std::string> HardLink(const std::string& source, const std::string& destination) {
    if (::link(source.c_str(), destination.c_str()) != 0) {
        return make_unexpected(Errno("hard_link", source, errno));
    }
    return Unit();
}

Expected<Unit, std::string> Copy(const std::string& source, const std::string& destination) {
    std::ifstream input(source.c_str(), std::ios::in | std::ios::binary);
    if (!input) return make_unexpected(Errno("copy (open source)", source, ENOENT));
    std::ofstream output(destination.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) return make_unexpected(Errno("copy (open destination)", destination, EACCES));
    output << input.rdbuf();
    output.flush();
    if (!output) return make_unexpected(Errno("copy", destination, EIO));
    return Unit();
}

Expected<uint64_t, std::string> FileSize(const std::string& path) {
    struct stat st;
    if (!Stat(path, &st)) return make_unexpected(Errno("metadata", path, errno));
    return static_cast<uint64_t>(st.st_size);
}

Expected<std::vector<std::string>, std::string> ReadDir(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == NULL) return make_unexpected(Errno("read_dir", path, errno));
    std::vector<std::string> names;
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    ::closedir(dir);
    return names;
}

Expected<std::string, std::string> CreateTempDir(const std::string& prefix) {
    const char* tmp_env = ::getenv("TMPDIR");
    const std::string base = (tmp_env != NULL && tmp_env[0] != '\0') ? tmp_env : "/tmp";
    std::string pattern = Join(base, prefix + "XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(&buffer[0]) == NULL) {
        return make_unexpected(Errno("create_temp_dir", pattern, errno));
    }
    return std::string(&buffer[0]);
}

Expected<std::string, std::string> CreateTempDirIn(const std::string& parent,
                                                   const std::string& prefix) {
    std::string pattern = Join(parent, prefix + "XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(&buffer[0]) == NULL) {
        return make_unexpected(Errno("create_temp_dir_in", pattern, errno));
    }
    return std::string(&buffer[0]);
}

// ---------------------------------------------------------------------------
// Unix security primitives
// ---------------------------------------------------------------------------

FileDescriptor::FileDescriptor(int fd) {
    if (fd < 0) return;
    // The deleter is what makes this RAII: the fd closes when the last copy of
    // the handle goes away. close() is not retried on EINTR, because on Linux
    // the descriptor is already gone and a retry could close an unrelated one.
    holder_.reset(new int(fd), [](int* owned) {
        if (*owned >= 0) ::close(*owned);
        delete owned;
    });
}

namespace {

FileStat FromStatBuf(const struct stat& st, bool used_lstat) {
    FileStat out;
    out.is_symlink = used_lstat && S_ISLNK(st.st_mode);
    out.is_dir = S_ISDIR(st.st_mode);
    out.is_regular = S_ISREG(st.st_mode);
    out.mode = static_cast<uint32_t>(st.st_mode) & 07777u;
    out.uid = static_cast<uint32_t>(st.st_uid);
    out.gid = static_cast<uint32_t>(st.st_gid);
    out.size = static_cast<uint64_t>(st.st_size);
    return out;
}

}  // namespace

Expected<FileStat, std::string> SymlinkStat(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return make_unexpected(Errno("symlink_metadata", path, errno));
    }
    return FromStatBuf(st, true);
}

Expected<FileStat, std::string> Stat(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return make_unexpected(Errno("metadata", path, errno));
    }
    return FromStatBuf(st, false);
}

Expected<FileStat, std::string> StatFd(int fd) {
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        return make_unexpected(Errno("fstat", "<fd>", errno));
    }
    return FromStatBuf(st, false);
}

Expected<FileDescriptor, std::string> OpenReadFollow(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return make_unexpected(Errno("open", path, errno));
    return FileDescriptor(fd);
}

Expected<FileDescriptor, std::string> OpenReadNoFollow(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return make_unexpected(Errno("open", path, errno));
    return FileDescriptor(fd);
}

Expected<Optional<FileDescriptor>, std::string> OpenReadNoFollowOptional(
    const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int saved = errno;  // captured immediately; nothing may intervene
        if (saved == ENOENT) return Optional<FileDescriptor>();
        return make_unexpected(Errno("open", path, saved));
    }
    return Optional<FileDescriptor>(FileDescriptor(fd));
}

Expected<Optional<FileDescriptor>, std::string> OpenReadFollowOptional(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int saved = errno;
        if (saved == ENOENT) return Optional<FileDescriptor>();
        return make_unexpected(Errno("open", path, saved));
    }
    return Optional<FileDescriptor>(FileDescriptor(fd));
}

Expected<Unit, std::string> WriteFd(int fd, const std::string& contents) {
    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t got = ::write(fd, contents.data() + written, contents.size() - written);
        if (got < 0) {
            if (errno == EINTR) continue;
            return make_unexpected(Errno("write", "<fd>", errno));
        }
        written += static_cast<std::size_t>(got);
    }
    return Unit();
}

Expected<std::string, std::string> ReadFdToString(int fd, std::size_t limit) {
    std::string out;
    out.reserve(limit < 4096 ? limit : 4096);

    char buffer[4096];
    while (out.size() < limit) {
        const std::size_t want = limit - out.size();
        const std::size_t chunk = want < sizeof(buffer) ? want : sizeof(buffer);
        const ssize_t got = ::read(fd, buffer, chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            return make_unexpected(Errno("read", "<fd>", errno));
        }
        if (got == 0) break;  // EOF
        out.append(buffer, static_cast<std::size_t>(got));
    }
    return out;
}

Expected<Unit, std::string> SetPermissions(const std::string& path, uint32_t mode) {
    if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) {
        return make_unexpected(Errno("set_permissions", path, errno));
    }
    return Unit();
}

Expected<Unit, std::string> CreateDirAllWithMode(const std::string& path, uint32_t mode) {
    if (path.empty()) {
        return make_unexpected(std::string("create_dir_all '': empty path"));
    }

    std::string progress;
    std::size_t cursor = 0;
    if (path[0] == '/') {
        progress = "/";
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string component =
            path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!component.empty()) {
            progress = Join(progress, component);
            if (::mkdir(progress.c_str(), static_cast<mode_t>(mode)) != 0) {
                if (errno != EEXIST) {
                    return make_unexpected(Errno("create_dir_all", progress, errno));
                }
            } else {
                // mkdir applies the umask, but `DirBuilderExt::mode` promises
                // the exact bits, so they are reasserted here.
                if (::chmod(progress.c_str(), static_cast<mode_t>(mode)) != 0) {
                    return make_unexpected(Errno("set_permissions", progress, errno));
                }
            }
        }
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }
    return Unit();
}

Expected<Unit, std::string> SyncFd(int fd) {
    if (::fsync(fd) != 0) return make_unexpected(Errno("sync_all", "<fd>", errno));
    return Unit();
}

Expected<Unit, std::string> SyncDirectory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return make_unexpected(Errno("open directory", path, errno));
    const bool synced = ::fsync(fd) == 0;
    const int saved = errno;
    ::close(fd);
    if (!synced) return make_unexpected(Errno("sync directory", path, saved));
    return Unit();
}

Expected<bool, std::string> PersistNoClobber(const std::string& source,
                                             const std::string& destination) {
    // link() fails with EEXIST rather than replacing, which is exactly the
    // no-clobber guarantee; rename() would silently overwrite.
    if (::link(source.c_str(), destination.c_str()) != 0) {
        const int saved = errno;
        if (saved == EEXIST) {
            // Someone else created it first. Drop our temporary and report the
            // loss so the caller can read the winner's file.
            ::unlink(source.c_str());
            return false;
        }
        return make_unexpected(Errno("persist", destination, saved));
    }
    if (::unlink(source.c_str()) != 0 && errno != ENOENT) {
        return make_unexpected(Errno("unlink temporary", source, errno));
    }
    return true;
}

Expected<TempFile, std::string> CreateTempFileIn(const std::string& dir,
                                                 const std::string& prefix) {
    std::string pattern = Join(dir, prefix + "XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');

    const int fd = ::mkstemp(&buffer[0]);
    if (fd < 0) return make_unexpected(Errno("create_temp_file", pattern, errno));

    TempFile out;
    out.fd = FileDescriptor(fd);
    out.path = std::string(&buffer[0]);
    return out;
}

uint32_t EffectiveUid() { return static_cast<uint32_t>(::geteuid()); }

bool EffectiveUidIsRoot() { return ::geteuid() == 0; }

}  // namespace fs
}  // namespace core
}  // namespace agentenv
