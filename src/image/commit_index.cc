// SPDX-License-Identifier: MIT
// Rust: src/image/commit_index.rs
#include "agentenv/image/commit_index.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <vector>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace image {

namespace {

const uint32_t    kIndexSchemaVersion = 1;
const char* const kOciSourceKind = "oci-layer";
const std::size_t kMaxDigestSlugLen = 160;

// ---- small filesystem helpers (POSIX) ----

bool FileMetadata(const std::string& path, bool* is_file, uint64_t* len) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    *is_file = S_ISREG(st.st_mode);
    *len = static_cast<uint64_t>(st.st_size);
    return true;
}

std::string ParentDir(const std::string& path) {
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
 return path.substr(0, slash);
}

// Rust `tokio::fs::create_dir_all` — mkdir -p.
core::Expected<core::Unit, std::string> CreateDirAll(const std::string& dir) {
 if (dir.empty() || dir == "." || dir == "/") return core::Unit{};
    std::string acc;
    std::string::size_type i = 0;
  if (dir[0] == '/') { acc = "/"; ++i; }
    while (i < dir.size()) {
        std::string::size_type slash = dir.find('/', i);
   std::string::size_type end =
(slash == std::string::npos) ? dir.size() : slash;
        std::string segment = dir.substr(i, end - i);
      if (!segment.empty()) {
            if (!acc.empty() && acc != "/") acc += "/";
  acc += segment;
      if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
    return core::make_unexpected<std::string>(
"create dir " + acc + ": " + std::strerror(errno));
            }
        }
        i = (slash == std::string::npos) ? dir.size() : slash + 1;
    }
    return core::Unit{};
}

std::string JsonEscape(const std::string& s) {
  std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
       case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
         case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
 case '\t': out += "\\t";  break;
  default:   out += c;    break;
    }
    }
    return out;
}

// Extract a JSON string value for `"key"`. Returns false if the key is absent.
bool JsonFindString(const std::string& json, const std::string& key,
             std::string* out) {
    std::string needle = "\"" + key + "\"";
    std::string::size_type k = json.find(needle);
    if (k == std::string::npos) return false;
    std::string::size_type colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    std::string::size_type q = json.find('"', colon + 1);
    if (q == std::string::npos) return false;
    std::string val;
    for (std::string::size_type i = q + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char n = json[++i];
      switch (n) {
        case 'n': val += '\n'; break;
      case 'r': val += '\r'; break;
  case 't': val += '\t'; break;
             default:  val += n; break;
            }
        } else if (c == '"') {
  *out = val;
            return true;
        } else {
    val += c;
        }
    }
    return false;
}

// Extract a numeric/boolean scalar for `"key"` as raw token text.
bool JsonFindScalar(const std::string& json, const std::string& key,
          std::string* out) {
    std::string needle = "\"" + key + "\"";
    std::string::size_type k = json.find(needle);
 if (k == std::string::npos) return false;
    std::string::size_type colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
 std::string::size_type i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
           json[i] == '\n' || json[i] == '\r')) {
  ++i;
    }
    std::string tok;
    while (i < json.size() && json[i] != ',' && json[i] != '}' &&
      json[i] != '\n' && json[i] != ' ') {
     tok += json[i++];
    }
    if (tok.empty()) return false;
    *out = tok;
    return true;
}

}  // namespace

const char* const kOverlaybdCommitFile = "overlaybd.commit";

// ---- CommitIndex ----

CommitIndex CommitIndex::OciLayer(std::string source_digest,
     std::string commit_digest,
uint64_t size,
  std::string converter,
     uint64_t virtual_size_gib,
        bool mkfs,
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
    const std::string& source_digest, const std::string& converter,
 uint64_t virtual_size_gib, bool mkfs,
    const core::Optional<std::string>& parent_commit_digest) const {
    bool parents_equal =
     (parent_commit_digest.has_value() == this->parent_commit_digest.has_value()) &&
 (!parent_commit_digest.has_value() ||
         *parent_commit_digest == *this->parent_commit_digest);
    return schema == kIndexSchemaVersion &&
  source_kind == kOciSourceKind &&
           this->source_digest == source_digest &&
  this->converter == converter &&
     this->virtual_size_gib == virtual_size_gib &&
  this->mkfs == mkfs &&
 parents_equal;
}

std::string CommitIndex::ToJsonPretty() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": " << schema << ",\n";
    os << "  \"sourceKind\": \"" << JsonEscape(source_kind) << "\",\n";
    os << "  \"sourceDigest\": \"" << JsonEscape(source_digest) << "\",\n";
    os << "  \"commitDigest\": \"" << JsonEscape(commit_digest) << "\",\n";
    os << "  \"size\": " << size << ",\n";
    os << "  \"converter\": \"" << JsonEscape(converter) << "\",\n";
    os << "  \"virtualSizeGib\": " << virtual_size_gib << ",\n";
    os << "  \"mkfs\": " << (mkfs ? "true" : "false");
    if (parent_commit_digest.has_value()) {
        os << ",\n  \"parentCommitDigest\": \""
           << JsonEscape(*parent_commit_digest) << "\"";
    }
    os << "\n}";
    return os.str();
}

core::Expected<CommitIndex, std::string> CommitIndex::FromJson(
    const std::string& json) {
    CommitIndex idx;
    std::string tok;
    if (!JsonFindScalar(json, "schema", &tok)) {
        return core::make_unexpected<std::string>("missing schema");
    }
    idx.schema = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (!JsonFindString(json, "sourceKind", &idx.source_kind) ||
        !JsonFindString(json, "sourceDigest", &idx.source_digest) ||
        !JsonFindString(json, "commitDigest", &idx.commit_digest) ||
        !JsonFindString(json, "converter", &idx.converter)) {
        return core::make_unexpected<std::string>("missing string field");
    }
    if (!JsonFindScalar(json, "size", &tok)) {
        return core::make_unexpected<std::string>("missing size");
    }
    idx.size = std::strtoull(tok.c_str(), nullptr, 10);
    if (!JsonFindScalar(json, "virtualSizeGib", &tok)) {
     return core::make_unexpected<std::string>("missing virtualSizeGib");
    }
    idx.virtual_size_gib = std::strtoull(tok.c_str(), nullptr, 10);
    if (!JsonFindScalar(json, "mkfs", &tok)) {
      return core::make_unexpected<std::string>("missing mkfs");
    }
    idx.mkfs = (tok == "true");
    std::string parent;
    if (JsonFindString(json, "parentCommitDigest", &parent)) {
        idx.parent_commit_digest = parent;
    }
    return idx;
}

core::Expected<core::Optional<CommitIndex>, std::string> CommitIndex::Read(
    const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
   // Absent file -> Ok(None); any other open failure would also land here,
  // but the caller treats a missing index as "recompute", matching Rust's
        // NotFound handling.
        return core::Optional<CommitIndex>();
    }
    std::ostringstream ss;
  ss << in.rdbuf();
    auto parsed = FromJson(ss.str());
    if (!parsed.ok()) {
        return core::make_unexpected<std::string>(
   "parse commit index " + path + ": " + parsed.error());
    }
    return core::Optional<CommitIndex>(parsed.take_value());
}

core::Expected<core::Unit, std::string> CommitIndex::Write(
  const std::string& path) const {
    std::string parent = ParentDir(path);
    auto mk = CreateDirAll(parent);
    if (!mk.ok()) {
        return core::make_unexpected<std::string>(
 "create commit index dir " + parent + ": " + mk.error());
    }
    std::string tmp = parent + "/.commit_index.tmp.XXXXXX";
    std::vector<char> buf(tmp.begin(), tmp.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        return core::make_unexpected<std::string>(
            "create temp commit index in " + parent + ": " +
        std::strerror(errno));
    }
    std::string tmp_path(buf.data());
    ::close(fd);
    {
 std::ofstream out(tmp_path.c_str(), std::ios::binary | std::ios::trunc);
        std::string json = ToJsonPretty();
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) {
            ::unlink(tmp_path.c_str());
 return core::make_unexpected<std::string>(
                "write temp commit index " + tmp_path);
        }
    }
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
  ::unlink(tmp_path.c_str());
 return core::make_unexpected<std::string>(
            "move commit index " + tmp_path + " to " + path + ": " +
      std::strerror(errno));
    }
 return core::Unit{};
}

bool CommitIndex::operator==(const CommitIndex& o) const {
    bool parents_equal =
      (parent_commit_digest.has_value() == o.parent_commit_digest.has_value()) &&
   (!parent_commit_digest.has_value() ||
         *parent_commit_digest == *o.parent_commit_digest);
    return schema == o.schema && source_kind == o.source_kind &&
      source_digest == o.source_digest && commit_digest == o.commit_digest &&
  size == o.size && converter == o.converter &&
    virtual_size_gib == o.virtual_size_gib && mkfs == o.mkfs &&
     parents_equal;
}

// ---- free functions ----

std::string SanitizeFilenameComponent(const std::string& value,
   std::size_t max_len) {
    std::string component;
    component.reserve(value.size());
    for (char c : value) {
     bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        component += ok ? c : '-';
    }
    if (component.size() > max_len) component.resize(max_len);
    return component;
}

std::string DigestSlug(const std::string& digest) {
    std::string slug;
    std::string::size_type colon = digest.find(':');
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

core::Optional<std::string> CachedCommitFileIfUsable(
    const std::string& commit_store, const std::string& digest, uint64_t size) {
    std::string path = CommitFile(commit_store, digest);
    bool is_file = false;
    uint64_t len = 0;
    if (FileMetadata(path, &is_file, &len) && is_file && len == size) {
        return path;
    }
    return core::nullopt;
}

core::Expected<std::string, std::string> AcceptExistingCommitFile(
    const std::string& destination, const std::string& digest, uint64_t size) {
    bool is_file = false;
    uint64_t len = 0;
 if (!FileMetadata(destination, &is_file, &len)) {
     return core::make_unexpected<std::string>(
            "stat existing commit cache file " + destination);
    }
    if (!is_file) {
    return core::make_unexpected<std::string>(
            "commit cache file " + destination + " for " + digest +
            " is not a regular file");
    }
    if (len != size) {
      std::ostringstream os;
    os << "commit cache file " << destination << " size mismatch for "
           << digest << ": expected " << size << ", found " << len;
        return core::make_unexpected<std::string>(os.str());
    }
    return destination;
}

namespace {

// Shared tail for both seed paths: verify size then persist a temp file into the
// destination without clobbering an existing (concurrently written) file.
core::Expected<std::string, std::string> PersistNoClobber(
    const std::string& tmp_path, const std::string& destination,
    const std::string& digest, uint64_t size) {
    if (::link(tmp_path.c_str(), destination.c_str()) == 0) {
        ::unlink(tmp_path.c_str());
        return destination;
    }
    if (errno == EEXIST) {
     ::unlink(tmp_path.c_str());
        return AcceptExistingCommitFile(destination, digest, size);
    }
    // rename() clobbers, so we used link()+unlink() for the noclobber semantics;
    // fall back to rename only when the destination genuinely does not exist.
    if (::rename(tmp_path.c_str(), destination.c_str()) == 0) {
        return destination;
}
    std::string err = std::strerror(errno);
    ::unlink(tmp_path.c_str());
    return core::make_unexpected<std::string>(
        "persist temp commit cache file " + tmp_path + " to " + destination +
        ": " + err);
}

core::Expected<std::string, std::string> MakeTempIn(const std::string& parent,
           std::string* out_path) {
    std::string tmpl = parent + "/.commit_cache.tmp.XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
if (fd < 0) {
        return core::make_unexpected<std::string>(
            "create temp commit cache file in " + parent + ": " +
      std::strerror(errno));
    }
    ::close(fd);
  *out_path = std::string(buf.data());
    return *out_path;
}

}  // namespace

core::Expected<std::string, std::string> SeedCommitFile(
    const std::string& commit_store, const std::string& source,
  const std::string& digest, uint64_t size) {
    std::string destination = CommitFile(commit_store, digest);
    auto existing = CachedCommitFileIfUsable(commit_store, digest, size);
    if (existing.has_value()) return *existing;

    std::string parent = ParentDir(destination);
    auto mk = CreateDirAll(parent);
    if (!mk.ok()) {
        return core::make_unexpected<std::string>(
   "create commit cache dir " + parent + ": " + mk.error());
    }

    bool is_file = false;
    uint64_t source_len = 0;
    if (!FileMetadata(source, &is_file, &source_len)) {
return core::make_unexpected<std::string>("stat source commit " + source);
    }
    if (source_len != size) {
   std::ostringstream os;
        os << "commit cache seed size mismatch for " << source
           << ": descriptor says " << size << ", file has " << source_len;
        return core::make_unexpected<std::string>(os.str());
  }

    // Copy source into a temp file while hashing (mirrors copy_with_sha256_hex).
    std::string tmp_path;
    auto mkt = MakeTempIn(parent, &tmp_path);
    if (!mkt.ok()) return core::make_unexpected(mkt.take_error());

std::ifstream in(source.c_str(), std::ios::binary);
    if (!in) {
      ::unlink(tmp_path.c_str());
        return core::make_unexpected<std::string>("open source commit " + source);
}
 std::ofstream out(tmp_path.c_str(), std::ios::binary | std::ios::trunc);
    std::string all((std::istreambuf_iterator<char>(in)),
      std::istreambuf_iterator<char>());
    out.write(all.data(), static_cast<std::streamsize>(all.size()));
    out.flush();
    if (!out) {
        ::unlink(tmp_path.c_str());
 return core::make_unexpected<std::string>(
            "copy source commit " + source + " to temp commit cache file " +
            tmp_path);
    }
    uint64_t copied = all.size();
    std::string actual_digest = core::Sha256Digest(all);

    if (copied != size) {
        ::unlink(tmp_path.c_str());
        std::ostringstream os;
        os << "short commit cache seed copy for " << source << ": expected "
           << size << " bytes, copied " << copied;
        return core::make_unexpected<std::string>(os.str());
    }
    if (actual_digest != digest) {
  ::unlink(tmp_path.c_str());
        return core::make_unexpected<std::string>(
            "commit cache seed digest mismatch for " + source + ": expected " +
            digest + ", got " + actual_digest);
    }
    return PersistNoClobber(tmp_path, destination, digest, size);
}

core::Expected<std::string, std::string> SeedCommitFileTrustedDescriptor(
    const std::string& commit_store, const std::string& source,
    const std::string& digest, uint64_t size) {
    std::string destination = CommitFile(commit_store, digest);
    auto existing = CachedCommitFileIfUsable(commit_store, digest, size);
    if (existing.has_value()) return *existing;

    std::string parent = ParentDir(destination);
    auto mk = CreateDirAll(parent);
    if (!mk.ok()) {
 return core::make_unexpected<std::string>(
     "create commit cache dir " + parent + ": " + mk.error());
    }

    bool is_file = false;
    uint64_t source_len = 0;
    if (!FileMetadata(source, &is_file, &source_len)) {
        return core::make_unexpected<std::string>("stat source commit " + source);
    }
    if (source_len != size) {
        std::ostringstream os;
        os << "trusted commit cache seed size mismatch for " << source
           << ": descriptor says " << size << ", file has " << source_len;
        return core::make_unexpected<std::string>(os.str());
    }

    // Fast path: hard-link the trusted source directly into the cache.
    if (::link(source.c_str(), destination.c_str()) == 0) {
     return destination;
    }
    if (errno == EEXIST) {
        return AcceptExistingCommitFile(destination, digest, size);
    }
    // Fall back to a verified copy (mirrors the debug-logged fallback in Rust).

    std::string tmp_path;
    auto mkt = MakeTempIn(parent, &tmp_path);
    if (!mkt.ok()) return core::make_unexpected(mkt.take_error());

    std::ifstream in(source.c_str(), std::ios::binary);
    if (!in) {
      ::unlink(tmp_path.c_str());
        return core::make_unexpected<std::string>(
        "copy trusted source commit " + source + " to temp " + tmp_path);
    }
    std::ofstream out(tmp_path.c_str(), std::ios::binary | std::ios::trunc);
    std::string all((std::istreambuf_iterator<char>(in)),
   std::istreambuf_iterator<char>());
    out.write(all.data(), static_cast<std::streamsize>(all.size()));
    out.flush();
    if (!out) {
 ::unlink(tmp_path.c_str());
        return core::make_unexpected<std::string>(
    "copy trusted source commit " + source + " to temp " + tmp_path);
    }
    if (all.size() != size) {
        ::unlink(tmp_path.c_str());
        std::ostringstream os;
        os << "trusted commit cache seed temp size mismatch for " << source
       << ": expected " << size;
        return core::make_unexpected<std::string>(os.str());
    }
    return PersistNoClobber(tmp_path, destination, digest, size);
}

std::string IndexPath(const std::string& index_dir,
     const std::string& source_digest,
     const std::string& converter,
  uint64_t virtual_size_gib,
       bool mkfs,
          const core::Optional<std::string>& parent_commit_digest) {
    // Compact JSON identical to serde_json::to_vec(IndexPathContext), key order
    // = declaration order, parentCommitDigest omitted only when None is a real
    // absence — serde serializes Option<None> as `null` here (no skip attr), so
    // we must emit `null` to keep the digest stable.
    std::ostringstream ctx;
    ctx << "{\"converter\":\"" << JsonEscape(converter) << "\","
        << "\"virtualSizeGib\":" << virtual_size_gib << ","
    << "\"mkfs\":" << (mkfs ? "true" : "false") << ","
      << "\"parentCommitDigest\":";
    if (parent_commit_digest.has_value()) {
        ctx << "\"" << JsonEscape(*parent_commit_digest) << "\"";
    } else {
    ctx << "null";
    }
    ctx << "}";
    std::string ctx_str = ctx.str();
    std::string ctx_hex = core::Sha256Hex(ctx_str);
    return index_dir + "/" + DigestSlug(source_digest) + "/sha256-" + ctx_hex +
    ".json";
}

}  // namespace image
}  // namespace agentenv
