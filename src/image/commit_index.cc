// SPDX-License-Identifier: MIT
// Rust: src/image/commit_index.rs
#include "agentenv/image/commit_index.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace image {

namespace {

const uint32_t    kIndexSchemaVersion = 1;
const char* const kOciSourceKind = "oci-layer";
const std::size_t kMaxDigestSlugLen = 160;

// ---- small filesystem helpers (POSIX) ----

/// Rust `fs::metadata` (follows symlinks). Returns the errno on failure.
int FileMetadata(const std::string& path, bool* is_file, uint64_t* len) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return errno;
    *is_file = S_ISREG(st.st_mode);
    *len = static_cast<uint64_t>(st.st_size);
    return 0;
}

std::string ParentDir(const std::string& path) {
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Rust `fs::create_dir_all` — mkdir -p.
core::Expected<core::Unit, std::string> CreateDirAll(const std::string& dir) {
    if (dir.empty() || dir == "." || dir == "/") return core::Unit{};
    std::string acc;
    std::string::size_type i = 0;
    if (dir[0] == '/') {
        acc = "/";
        ++i;
    }
    while (i < dir.size()) {
        std::string::size_type slash = dir.find('/', i);
        std::string::size_type end = (slash == std::string::npos) ? dir.size() : slash;
        std::string segment = dir.substr(i, end - i);
        if (!segment.empty()) {
            if (!acc.empty() && acc != "/") acc += "/";
            acc += segment;
            if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
                return core::make_unexpected<std::string>("create dir " + acc + ": " +
                                                          std::strerror(errno));
            }
        }
        i = (slash == std::string::npos) ? dir.size() : slash + 1;
    }
    return core::Unit{};
}

// ---- serde field readers for `CommitIndex` ----
//
// `CommitIndex` derives `Deserialize` with no `#[serde(default)]`: every field
// but the `Option` is required, and each must have the right JSON type. A
// substring scan for `"key":` would accept a document serde rejects (and pick
// up a key nested anywhere in the text), so the parse goes through a real
// JSON value.

core::Expected<const core::Json*, std::string> Required(const core::Json& object,
                                                       const char* key) {
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    if (found == map.end()) {
        return core::make_unexpected(std::string("missing field `") + key + "`");
    }
    return &found->second;
}

core::Expected<std::string, std::string> RequiredString(const core::Json& object,
                                                        const char* key) {
    const core::Expected<const core::Json*, std::string> field = Required(object, key);
    if (!field.ok()) return core::make_unexpected(field.error());
    if (field.value()->kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("invalid type for `") + key +
                                     "`: expected a string");
    }
    return field.value()->as_string();
}

core::Expected<uint64_t, std::string> RequiredUnsigned(const core::Json& object, const char* key,
                                                       uint64_t max) {
    const core::Expected<const core::Json*, std::string> field = Required(object, key);
    if (!field.ok()) return core::make_unexpected(field.error());
    const core::Json& value = *field.value();
    if (value.kind() != core::Json::Kind::Int || value.as_int() < 0 ||
        static_cast<uint64_t>(value.as_int()) > max) {
        return core::make_unexpected(std::string("invalid value for `") + key +
                                     "`: expected an unsigned integer");
    }
    return static_cast<uint64_t>(value.as_int());
}

core::Expected<bool, std::string> RequiredBool(const core::Json& object, const char* key) {
    const core::Expected<const core::Json*, std::string> field = Required(object, key);
    if (!field.ok()) return core::make_unexpected(field.error());
    if (field.value()->kind() != core::Json::Kind::Bool) {
        return core::make_unexpected(std::string("invalid type for `") + key +
                                     "`: expected a boolean");
    }
    return field.value()->as_bool();
}

}  // namespace

const char* const kOverlaybdCommitFile = "overlaybd.commit";

// ---- CommitIndex ----

CommitIndex CommitIndex::OciLayer(std::string source_digest, std::string commit_digest,
                                  uint64_t size, std::string converter,
                                  uint64_t virtual_size_gib, bool mkfs,
                                  core::Optional<std::string> parent_commit_digest) {
    CommitIndex idx;
    idx.schema = kIndexSchemaVersion;
    idx.source_kind = kOciSourceKind;
    idx.source_digest = std::move(source_digest);
    idx.commit_digest = std::move(commit_digest);
    idx.size = size;
    idx.converter = std::move(converter);
    idx.virtual_size_gib = virtual_size_gib;
    idx.mkfs = mkfs;
    idx.parent_commit_digest = std::move(parent_commit_digest);
    return idx;
}

bool CommitIndex::MatchesOciContext(
    const std::string& source_digest, const std::string& converter, uint64_t virtual_size_gib,
    bool mkfs, const core::Optional<std::string>& parent_commit_digest) const {
    const bool parents_equal =
        (parent_commit_digest.has_value() == this->parent_commit_digest.has_value()) &&
        (!parent_commit_digest.has_value() ||
         *parent_commit_digest == *this->parent_commit_digest);
    return schema == kIndexSchemaVersion && source_kind == kOciSourceKind &&
           this->source_digest == source_digest && this->converter == converter &&
           this->virtual_size_gib == virtual_size_gib && this->mkfs == mkfs && parents_equal;
}

std::string CommitIndex::ToJsonPretty() const {
    // `serde_json::to_vec_pretty`: struct fields in declaration order (which
    // `core::JsonObject`, a sorted map, cannot keep), `"key": value`, two-space
    // indent, and `parentCommitDigest` skipped when `None`.
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": " << schema << ",\n";
    os << "  \"sourceKind\": " << core::JsonQuote(source_kind) << ",\n";
    os << "  \"sourceDigest\": " << core::JsonQuote(source_digest) << ",\n";
    os << "  \"commitDigest\": " << core::JsonQuote(commit_digest) << ",\n";
    os << "  \"size\": " << size << ",\n";
    os << "  \"converter\": " << core::JsonQuote(converter) << ",\n";
    os << "  \"virtualSizeGib\": " << virtual_size_gib << ",\n";
    os << "  \"mkfs\": " << (mkfs ? "true" : "false");
    if (parent_commit_digest.has_value()) {
        os << ",\n  \"parentCommitDigest\": " << core::JsonQuote(*parent_commit_digest);
    }
    os << "\n}";
    return os.str();
}

core::Expected<CommitIndex, std::string> CommitIndex::FromJson(const std::string& json) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(json);
    if (!parsed.ok()) return core::make_unexpected(parsed.error().chain());
    const core::Json& value = parsed.value();
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct CommitIndex"));
    }

    CommitIndex idx;
    const core::Expected<uint64_t, std::string> schema =
        RequiredUnsigned(value, "schema", std::numeric_limits<uint32_t>::max());
    if (!schema.ok()) return core::make_unexpected(schema.error());
    idx.schema = static_cast<uint32_t>(schema.value());

    const char* const kStringKeys[] = {"sourceKind", "sourceDigest", "commitDigest",
                                       "converter"};
    std::string* const string_fields[] = {&idx.source_kind, &idx.source_digest,
                                          &idx.commit_digest, &idx.converter};
    for (std::size_t i = 0; i < 4; ++i) {
        const core::Expected<std::string, std::string> field =
            RequiredString(value, kStringKeys[i]);
        if (!field.ok()) return core::make_unexpected(field.error());
        *string_fields[i] = field.value();
    }

    const uint64_t kU64Max = std::numeric_limits<uint64_t>::max();
    const core::Expected<uint64_t, std::string> size = RequiredUnsigned(value, "size", kU64Max);
    if (!size.ok()) return core::make_unexpected(size.error());
    idx.size = size.value();
    const core::Expected<uint64_t, std::string> virtual_size =
        RequiredUnsigned(value, "virtualSizeGib", kU64Max);
    if (!virtual_size.ok()) return core::make_unexpected(virtual_size.error());
    idx.virtual_size_gib = virtual_size.value();
    const core::Expected<bool, std::string> mkfs = RequiredBool(value, "mkfs");
    if (!mkfs.ok()) return core::make_unexpected(mkfs.error());
    idx.mkfs = mkfs.value();

    // `Option<String>`: absent and `null` are both `None`.
    const core::JsonObject& map = value.as_object();
    const core::JsonObject::const_iterator parent = map.find("parentCommitDigest");
    if (parent != map.end() && parent->second.kind() != core::Json::Kind::Null) {
        if (parent->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(
                std::string("invalid type for `parentCommitDigest`: expected a string"));
        }
        idx.parent_commit_digest = parent->second.as_string();
    }
    return idx;
}

core::Expected<core::Optional<CommitIndex>, std::string> CommitIndex::Read(
    const std::string& path) {
    // Only NotFound means "no index"; any other read failure (EACCES, EISDIR,
    // EIO) is an error, as in Rust. Folding those into `None` would make the
    // caller reconvert a layer whose index it merely could not read.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return core::Optional<CommitIndex>();
        return core::make_unexpected<std::string>("read commit index " + path + ": " +
                                                  std::strerror(errno));
    }
    std::string bytes;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
            ::close(fd);
            return core::make_unexpected<std::string>("read commit index " + path + ": " + why);
        }
        if (n == 0) break;
        bytes.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    core::Expected<CommitIndex, std::string> parsed = FromJson(bytes);
    if (!parsed.ok()) {
        return core::make_unexpected<std::string>("parse commit index " + path + ": " +
                                                  parsed.error());
    }
    return core::Optional<CommitIndex>(parsed.take_value());
}

namespace {

bool WriteAll(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

/// Rust `tempfile::NamedTempFile::new_in` — an open temp file in `parent`
/// (mode 0600, `.tmp` + random suffix) that is unlinked on drop unless
/// persisted.
class SeedTempFile {
 public:
    SeedTempFile() : fd_(-1) {}
    ~SeedTempFile() {
        if (fd_ >= 0) ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    SeedTempFile(const SeedTempFile&) = delete;
    SeedTempFile& operator=(const SeedTempFile&) = delete;

    /// Returns the errno on failure.
    int Create(const std::string& parent) {
        std::string tmpl = parent + "/.tmpXXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        const int fd = ::mkostemp(buf.data(), O_CLOEXEC);
        if (fd < 0) return errno;
        fd_ = fd;
        path_ = std::string(buf.data());
        return 0;
    }

    int fd() const { return fd_; }
    const std::string& path() const { return path_; }

    /// Rust `NamedTempFile::persist` — `rename(2)`, replacing the target.
    int Persist(const std::string& destination) {
        CloseFd();
        if (::rename(path_.c_str(), destination.c_str()) != 0) return errno;
        path_.clear();
        return 0;
    }

    /// Rust `NamedTempFile::persist_noclobber` (tempfile's unix `persist`
    /// with `overwrite = false`): `renameat2(RENAME_NOREPLACE)`, falling back
    /// to `link` + `unlink` only where renameat2 is unsupported. Never a plain
    /// `rename`, which would clobber a concurrently persisted destination.
    /// Returns the errno of the failure; EEXIST means "already there".
    int PersistNoClobber(const std::string& destination) {
        CloseFd();
#ifdef SYS_renameat2
        const long rc = ::syscall(SYS_renameat2, AT_FDCWD, path_.c_str(), AT_FDCWD,
                                  destination.c_str(), 1 /* RENAME_NOREPLACE */);
        if (rc == 0) {
            path_.clear();
            return 0;
        }
        if (errno != EINVAL && errno != ENOSYS) return errno;
#endif
        if (::link(path_.c_str(), destination.c_str()) != 0) return errno;
        ::unlink(path_.c_str());
        path_.clear();
        return 0;
    }

 private:
    void CloseFd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_;
    std::string path_;
};

}  // namespace

core::Expected<core::Unit, std::string> CommitIndex::Write(const std::string& path) const {
    const std::string parent = ParentDir(path);
    const core::Expected<core::Unit, std::string> mk = CreateDirAll(parent);
    if (!mk.ok()) {
        return core::make_unexpected<std::string>("create commit index dir " + parent + ": " +
                                                  mk.error());
    }
    SeedTempFile tmp;
    const int created = tmp.Create(parent);
    if (created != 0) {
        return core::make_unexpected<std::string>("create temp commit index in " + parent + ": " +
                                                  std::strerror(created));
    }
    const std::string json = ToJsonPretty();
    if (!WriteAll(tmp.fd(), json.data(), json.size())) {
        return core::make_unexpected<std::string>("write temp commit index " + tmp.path() + ": " +
                                                  std::strerror(errno));
    }
    const std::string tmp_path = tmp.path();
    const int persisted = tmp.Persist(path);
    if (persisted != 0) {
        return core::make_unexpected<std::string>("move commit index " + tmp_path + " to " +
                                                  path + ": " + std::strerror(persisted));
    }
    return core::Unit{};
}

bool CommitIndex::operator==(const CommitIndex& o) const {
    const bool parents_equal =
        (parent_commit_digest.has_value() == o.parent_commit_digest.has_value()) &&
        (!parent_commit_digest.has_value() || *parent_commit_digest == *o.parent_commit_digest);
    return schema == o.schema && source_kind == o.source_kind &&
           source_digest == o.source_digest && commit_digest == o.commit_digest &&
           size == o.size && converter == o.converter &&
           virtual_size_gib == o.virtual_size_gib && mkfs == o.mkfs && parents_equal;
}

// ---- free functions ----

std::string SanitizeFilenameComponent(const std::string& value, std::size_t max_len) {
    // Rust maps per `char`, so one multi-byte UTF-8 character becomes a single
    // '-', not one per byte. Continuation bytes (10xxxxxx) are therefore
    // folded into the '-' already emitted for their lead byte.
    std::string component;
    component.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if ((c & 0xC0) == 0x80) continue;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        component += ok ? static_cast<char>(c) : '-';
    }
    if (component.size() > max_len) component.resize(max_len);
    return component;
}

std::string DigestSlug(const std::string& digest) {
    std::string slug;
    const std::string::size_type colon = digest.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < digest.size()) {
        slug = digest.substr(0, colon) + "-" + digest.substr(colon + 1);
    } else {
        slug = "unknown-" + digest;
    }
    return SanitizeFilenameComponent(slug, kMaxDigestSlugLen);
}

std::string CommitDir(const std::string& commit_store, const std::string& digest) {
    return commit_store + "/" + DigestSlug(digest);
}

std::string CommitFile(const std::string& commit_store, const std::string& digest) {
    return CommitDir(commit_store, digest) + "/" + kOverlaybdCommitFile;
}

core::Optional<std::string> CachedCommitFileIfUsable(const std::string& commit_store,
                                                     const std::string& digest, uint64_t size) {
    const std::string path = CommitFile(commit_store, digest);
    bool is_file = false;
    uint64_t len = 0;
    if (FileMetadata(path, &is_file, &len) == 0 && is_file && len == size) return path;
    return core::nullopt;
}

core::Expected<std::string, std::string> AcceptExistingCommitFile(const std::string& destination,
                                                                  const std::string& digest,
                                                                  uint64_t size) {
    bool is_file = false;
    uint64_t len = 0;
    const int err = FileMetadata(destination, &is_file, &len);
    if (err != 0) {
        return core::make_unexpected<std::string>("stat existing commit cache file " +
                                                  destination + ": " + std::strerror(err));
    }
    if (!is_file) {
        return core::make_unexpected<std::string>("commit cache file " + destination + " for " +
                                                  digest + " is not a regular file");
    }
    if (len != size) {
        std::ostringstream os;
        os << "commit cache file " << destination << " size mismatch for " << digest
           << ": expected " << size << ", found " << len;
        return core::make_unexpected<std::string>(os.str());
    }
    return destination;
}

namespace {

core::Expected<std::string, std::string> FinishPersist(SeedTempFile* temp,
                                                       const std::string& destination,
                                                       const std::string& digest,
                                                       uint64_t size) {
    const std::string temp_path = temp->path();
    const int err = temp->PersistNoClobber(destination);
    if (err == 0) return destination;
    if (err == EEXIST) return AcceptExistingCommitFile(destination, digest, size);
    return core::make_unexpected<std::string>("persist temp commit cache file " + temp_path +
                                              " to " + destination + ": " +
                                              std::strerror(err));
}

/// The shared prologue of both seed paths: create the commit dir, then check
/// the source length against the descriptor.
core::Expected<std::string, std::string> PrepareSeed(const std::string& destination,
                                                     const std::string& source, uint64_t size,
                                                     const char* mismatch_prefix) {
    const std::string parent = ParentDir(destination);
    const core::Expected<core::Unit, std::string> mk = CreateDirAll(parent);
    if (!mk.ok()) {
        return core::make_unexpected<std::string>("create commit cache dir " + parent + ": " +
                                                  mk.error());
    }
    bool is_file = false;
    uint64_t source_len = 0;
    const int err = FileMetadata(source, &is_file, &source_len);
    if (err != 0) {
        return core::make_unexpected<std::string>("stat source commit " + source + ": " +
                                                  std::strerror(err));
    }
    if (source_len != size) {
        std::ostringstream os;
        os << mismatch_prefix << " size mismatch for " << source << ": descriptor says " << size
           << ", file has " << source_len;
        return core::make_unexpected<std::string>(os.str());
    }
    return parent;
}

}  // namespace

core::Expected<std::string, std::string> SeedCommitFile(const std::string& commit_store,
                                                        const std::string& source,
                                                        const std::string& digest,
                                                        uint64_t size) {
    const std::string destination = CommitFile(commit_store, digest);
    const core::Optional<std::string> existing =
        CachedCommitFileIfUsable(commit_store, digest, size);
    if (existing.has_value()) return *existing;

    const core::Expected<std::string, std::string> prepared =
        PrepareSeed(destination, source, size, "commit cache seed");
    if (!prepared.ok()) return core::make_unexpected(prepared.error());
    const std::string& parent = prepared.value();

    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return core::make_unexpected<std::string>("open source commit " + source + ": " +
                                                  std::strerror(errno));
    }
    SeedTempFile temp;
    const int created = temp.Create(parent);
    if (created != 0) {
        ::close(in);
        return core::make_unexpected<std::string>("create temp commit cache file in " + parent +
                                                  ": " + std::strerror(created));
    }

    // Rust `digest::copy_with_sha256_hex`: stream the copy and hash the same
    // bytes, so neither side has to hold the whole layer in memory.
    core::Sha256Stream hasher;
    uint64_t copied = 0;
    std::vector<char> buf(128 * 1024);
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
            ::close(in);
            return core::make_unexpected<std::string>("copy source commit " + source +
                                                      " to temp commit cache file " +
                                                      temp.path() + ": " + why);
        }
        if (n == 0) break;
        if (!WriteAll(temp.fd(), buf.data(), static_cast<std::size_t>(n))) {
            const std::string why = std::strerror(errno);
            ::close(in);
            return core::make_unexpected<std::string>("copy source commit " + source +
                                                      " to temp commit cache file " +
                                                      temp.path() + ": " + why);
        }
        hasher.Update(buf.data(), static_cast<std::size_t>(n));
        copied += static_cast<uint64_t>(n);
    }
    ::close(in);
    if (::fsync(temp.fd()) != 0) {
        return core::make_unexpected<std::string>("sync temp commit cache file " +
                                                  temp.path() + ": " + std::strerror(errno));
    }
    const std::string actual_digest = hasher.PeekHex();

    if (copied != size) {
        std::ostringstream os;
        os << "short commit cache seed copy for " << source << ": expected " << size
           << " bytes, copied " << copied;
        return core::make_unexpected<std::string>(os.str());
    }
    if (actual_digest != digest) {
        return core::make_unexpected<std::string>("commit cache seed digest mismatch for " +
                                                  source + ": expected " + digest + ", got " +
                                                  actual_digest);
    }
    return FinishPersist(&temp, destination, digest, size);
}

core::Expected<std::string, std::string> SeedCommitFileTrustedDescriptor(
    const std::string& commit_store, const std::string& source, const std::string& digest,
    uint64_t size) {
    const std::string destination = CommitFile(commit_store, digest);
    const core::Optional<std::string> existing =
        CachedCommitFileIfUsable(commit_store, digest, size);
    if (existing.has_value()) return *existing;

    const core::Expected<std::string, std::string> prepared =
        PrepareSeed(destination, source, size, "trusted commit cache seed");
    if (!prepared.ok()) return core::make_unexpected(prepared.error());
    const std::string& parent = prepared.value();

    if (::link(source.c_str(), destination.c_str()) == 0) return destination;
    if (errno == EEXIST) return AcceptExistingCommitFile(destination, digest, size);
    AGENTENV_DEBUG("hard-link overlaybd commit cache seed failed; falling back to copy: source="
                   << source << " destination=" << destination
                   << " error=" << std::strerror(errno));

    SeedTempFile temp;
    const int created = temp.Create(parent);
    if (created != 0) {
        return core::make_unexpected<std::string>("create temp commit cache file in " + parent +
                                                  ": " + std::strerror(created));
    }

    // Rust `fs::copy(source, temp.path())`: contents *and* permission bits.
    // std's copy reopens the destination and `fchmod`s it to the source mode,
    // so the cached commit ends up with the source's permissions rather than
    // the temp file's 0600.
    const std::string copy_error_prefix =
        "copy trusted source commit " + source + " to temp " + temp.path() + ": ";
    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return core::make_unexpected(copy_error_prefix + std::strerror(errno));
    struct stat source_st;
    if (::fstat(in, &source_st) != 0) {
        const std::string why = std::strerror(errno);
        ::close(in);
        return core::make_unexpected(copy_error_prefix + why);
    }
    std::vector<char> buf(128 * 1024);
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
            ::close(in);
            return core::make_unexpected(copy_error_prefix + why);
        }
        if (n == 0) break;
        if (!WriteAll(temp.fd(), buf.data(), static_cast<std::size_t>(n))) {
            const std::string why = std::strerror(errno);
            ::close(in);
            return core::make_unexpected(copy_error_prefix + why);
        }
    }
    ::close(in);
    if (::fchmod(temp.fd(), source_st.st_mode & 07777) != 0) {
        return core::make_unexpected(copy_error_prefix + std::strerror(errno));
    }

    struct stat st;
    if (::fstat(temp.fd(), &st) != 0) {
        return core::make_unexpected<std::string>("stat temp commit cache file " +
                                                  temp.path() + ": " + std::strerror(errno));
    }
    if (static_cast<uint64_t>(st.st_size) != size) {
        std::ostringstream os;
        os << "trusted commit cache seed temp size mismatch for " << source << ": expected "
           << size;
        return core::make_unexpected<std::string>(os.str());
    }
    if (::fsync(temp.fd()) != 0) {
        return core::make_unexpected<std::string>("sync temp commit cache file " +
                                                  temp.path() + ": " + std::strerror(errno));
    }
    return FinishPersist(&temp, destination, digest, size);
}

std::string IndexPath(const std::string& index_dir, const std::string& source_digest,
                      const std::string& converter, uint64_t virtual_size_gib, bool mkfs,
                      const core::Optional<std::string>& parent_commit_digest) {
    // Compact JSON identical to `serde_json::to_vec(IndexPathContext)`: key
    // order is declaration order, and `parentCommitDigest` has no
    // `skip_serializing_if`, so `None` is written as `null`. Any byte of
    // difference changes the digest and orphans every existing index.
    std::ostringstream ctx;
    ctx << "{\"converter\":" << core::JsonQuote(converter) << ","
        << "\"virtualSizeGib\":" << virtual_size_gib << ","
        << "\"mkfs\":" << (mkfs ? "true" : "false") << ","
        << "\"parentCommitDigest\":";
    if (parent_commit_digest.has_value()) {
        ctx << core::JsonQuote(*parent_commit_digest);
    } else {
        ctx << "null";
    }
    ctx << "}";
    return index_dir + "/" + DigestSlug(source_digest) + "/sha256-" +
           core::Sha256Hex(ctx.str()) + ".json";
}

}  // namespace image
}  // namespace agentenv
