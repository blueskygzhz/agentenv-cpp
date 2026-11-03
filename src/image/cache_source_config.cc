// SPDX-License-Identifier: MIT
// Rust: src/image/cache/source_config.rs
#include "agentenv/image/cache_source_config.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/cache.h"
#include "agentenv/image/cache_metadata_store.h"
#include "agentenv/image/commit_index.h"

namespace agentenv {
namespace image {
namespace cache {

const std::size_t kMaxSourceImageScopeComponentLen = 96;
const std::size_t kSourceImageScopeHashHexLen = 16;

// ---- lock registry ----------------------------------------------------------

SourceImageLock::SourceImageLock(std::shared_ptr<SourceImageCache> cache, const std::string& key,
                                 std::shared_ptr<std::mutex> lock)
    : cache_(std::move(cache)), key_(key), lock_(std::move(lock)) {}

SourceImageLock::~SourceImageLock() {
    lock_->unlock();
    cache_->Release(key_, lock_);
}

std::shared_ptr<SourceImageLock> SourceImageCache::AcquireLock(
    const std::string& image_config_path) {
    const std::string key = StablePathIdentity(image_config_path);
    std::shared_ptr<std::mutex> lock;
    {
        std::lock_guard<std::mutex> guard(mu_);
        std::shared_ptr<std::mutex>& slot = locks_[key];
        if (!slot) slot = std::make_shared<std::mutex>();
        lock = slot;
    }
    // Outside the registry lock: waiting here must not stall other paths.
    lock->lock();
    return std::shared_ptr<SourceImageLock>(new SourceImageLock(shared_from_this(), key, lock));
}

void SourceImageCache::Release(const std::string& key, const std::shared_ptr<std::mutex>& lock) {
    std::lock_guard<std::mutex> guard(mu_);
    std::map<std::string, std::shared_ptr<std::mutex> >::iterator it = locks_.find(key);
    // Rust `remove_if(ptr_eq && strong_count <= 2)`: the registry's reference
    // plus the releasing holder's. A waiter has copied the pointer under `mu_`
    // already, so it keeps the entry alive.
    if (it != locks_.end() && it->second == lock && it->second.use_count() <= 2) {
        locks_.erase(it);
    }
}

std::size_t SourceImageCache::RegisteredLocksForTesting() const {
    std::lock_guard<std::mutex> guard(mu_);
    return locks_.size();
}

// ---- source key -------------------------------------------------------------

core::Expected<ImageCacheSourceImageKey, std::string> ImageCacheSourceImageKey::New(
    const std::string& manifest_digest, const core::Optional<std::string>& repository_scope) {
    ImageCacheSourceImageKey key;
    if (repository_scope.has_value()) {
        const std::string trimmed = core::TrimUnicodeWhitespace(*repository_scope);
        if (!trimmed.empty()) key.repository_scope_ = core::Optional<std::string>(trimmed);
    }
    const core::Expected<std::string, std::string> digest =
        NonEmpty("image cache source manifest digest", manifest_digest);
    if (!digest.ok()) return core::make_unexpected(digest.error());
    key.manifest_digest_ = digest.value();
    return key;
}

// ---- paths ------------------------------------------------------------------

namespace {

bool FileExistsNonempty(const std::string& path) {
    const core::Expected<core::fs::FileStat, std::string> stat = core::fs::Stat(path);
    return stat.ok() && stat.value().size > 0;
}

/// Rust `source_image_scope_component`.
core::Optional<std::string> SourceImageScopeComponent(const std::string& raw_scope) {
    const std::string scope = core::TrimUnicodeWhitespace(raw_scope);
    if (scope.empty()) return core::Optional<std::string>();
    std::string component = SanitizeFilenameComponent(scope, static_cast<std::size_t>(-1));
    if (component.empty()) return core::Optional<std::string>();
    if (component.size() <= kMaxSourceImageScopeComponentLen) {
        return core::Optional<std::string>(component);
    }
    const std::string hash = core::Sha256Hex(scope);
    const std::string suffix = hash.substr(0, kSourceImageScopeHashHexLen);
    const std::size_t prefix_len = kMaxSourceImageScopeComponentLen > kSourceImageScopeHashHexLen + 1
                                       ? kMaxSourceImageScopeComponentLen -
                                             (kSourceImageScopeHashHexLen + 1)
                                       : 0;
    component.resize(prefix_len);
    return core::Optional<std::string>(component + "-" + suffix);
}

/// Rust `Path::file_stem`: the file name up to its last `.`, unless that dot
/// is the leading one of a dotfile.
core::Optional<std::string> FileStem(const std::string& path) {
    const core::Optional<std::string> name = core::fs::FileName(path);
    if (!name.has_value()) return core::Optional<std::string>();
    if (*name == "..") return name;
    const std::size_t dot = name->rfind('.');
    if (dot == std::string::npos || dot == 0) return name;
    return core::Optional<std::string>(name->substr(0, dot));
}

core::Expected<bool, std::string> SourceImageConfigUsable(const std::string& path) {
    if (!FileExistsNonempty(path)) return false;

    const core::Expected<CacheOwnedImageConfigFacts, std::string> facts =
        LoadCacheOwnedImageConfigFacts(path);
    if (!facts.ok()) return core::make_unexpected(facts.error());
    for (std::size_t i = 0; i < facts.value().local_file_lowers.size(); ++i) {
        const CacheOwnedLocalFileLower& lower = facts.value().local_file_lowers[i];
        const core::Expected<core::fs::FileStat, std::string> stat = core::fs::Stat(lower.file);
        if (!stat.ok()) {
            return core::make_unexpected("stat local lower " + lower.file + ": " + stat.error());
        }
        if (!stat.value().is_regular || stat.value().size == 0) {
            return core::make_unexpected("local lower " + lower.file +
                                         " is empty or not a file");
        }
        if (stat.value().size != lower.size) {
            return core::make_unexpected("local lower " + lower.file +
                                         " size mismatch: expected " +
                                         std::to_string(lower.size) + ", got " +
                                         std::to_string(stat.value().size));
        }
    }
    if (facts.value().upper_file.has_value() && !FileExistsNonempty(*facts.value().upper_file)) {
        return core::make_unexpected("local upper " + *facts.value().upper_file +
                                     " is missing or empty");
    }
    return true;
}

}  // namespace

core::Expected<core::Optional<ImageResolutionMetadata>, std::string> ReadSourceImageMetadata(
    const std::string& metadata_path) {
    typedef core::Optional<ImageResolutionMetadata> Result;
    if (!FileExistsNonempty(metadata_path)) return Result();
    const core::Expected<std::string, std::string> bytes = core::fs::ReadToString(metadata_path);
    if (!bytes.ok()) {
        return core::make_unexpected("read image metadata " + metadata_path + ": " +
                                     bytes.error());
    }
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(bytes.value());
    if (!value.ok()) {
        return core::make_unexpected("parse image metadata " + metadata_path + ": " +
                                     value.error().chain());
    }
    const core::Expected<ImageResolutionMetadata, std::string> metadata =
        ParseImageResolutionMetadata(value.value());
    if (!metadata.ok()) {
        return core::make_unexpected("parse image metadata " + metadata_path + ": " +
                                     metadata.error());
    }
    return Result(metadata.value());
}

std::string SourceImageConfigFilename(const ImageCacheSourceImageKey& key) {
    std::string slug = SanitizeFilenameComponent(
        core::TrimUnicodeWhitespace(key.manifest_digest()), static_cast<std::size_t>(-1));
    if (slug.empty()) slug = "image";
    if (key.repository_scope().has_value()) {
        const core::Optional<std::string> scope =
            SourceImageScopeComponent(*key.repository_scope());
        if (scope.has_value()) return slug + "-" + *scope + "-image.json";
    }
    return slug + "-image.json";
}

std::string SourceImageMetadataPathForConfigPath(const std::string& path) {
    const core::Optional<std::string> stem = FileStem(path);
    const std::string name = (stem.has_value() ? *stem : std::string("image")) + ".metadata.json";
    // Rust `set_file_name`: replaces the last component, or appends when the
    // path has none.
    const core::Optional<std::string> parent = core::fs::Parent(path);
    if (!core::fs::FileName(path).has_value() || !parent.has_value()) {
        return core::fs::Join(path, name);
    }
    return core::fs::Join(*parent, name);
}

bool SourceImageConfigIsUsable(const std::string& path) {
    const core::Expected<bool, std::string> usable = SourceImageConfigUsable(path);
    if (usable.ok()) return usable.value();
    AGENTENV_WARN("cached image config is unusable; regenerating: config="
                  << path << " error=" << usable.error());
    return false;
}

core::Expected<core::Unit, std::string> WriteJsonAtomic(const std::string& path,
                                                        const core::Json& value,
                                                        const std::string& label) {
    const core::Optional<std::string> parent = core::fs::Parent(path);
    if (!parent.has_value()) {
        return core::make_unexpected(label + " path has no parent: " + path);
    }
    const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(*parent);
    if (!created.ok()) {
        return core::make_unexpected("create " + label + " parent " + *parent + ": " +
                                     created.error());
    }
    const core::Optional<std::string> name = core::fs::FileName(path);
    const std::string tmp_path =
        core::fs::Join(*parent, (name.has_value() ? *name : label) + ".tmp");
    const core::Expected<core::Unit, std::string> written =
        core::fs::Write(tmp_path, value.ToStringPretty());
    if (!written.ok()) {
        return core::make_unexpected("write temp " + label + " " + tmp_path + ": " +
                                     written.error());
    }
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        const int err = errno;
        return core::make_unexpected("move " + label + " " + tmp_path + " to " + path + ": " +
                                     std::strerror(err));
    }
    return core::Unit();
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
