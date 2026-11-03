// SPDX-License-Identifier: MIT
// Rust: src/image/cache/source_config.rs — source-image config naming, the
// per-config in-process lock, and the usability check that decides whether a
// cached config can be served or must be regenerated.
#ifndef AGENTENV_IMAGE_CACHE_SOURCE_CONFIG_H_
#define AGENTENV_IMAGE_CACHE_SOURCE_CONFIG_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {
namespace cache {

/// Rust `MAX_SOURCE_IMAGE_SCOPE_COMPONENT_LEN` / `SOURCE_IMAGE_SCOPE_HASH_HEX_LEN`.
extern const std::size_t kMaxSourceImageScopeComponentLen;
extern const std::size_t kSourceImageScopeHashHexLen;

class SourceImageCache;

/// Rust `SourceImageLock` — held for the lifetime of the object. On release
/// the registry entry is dropped once nobody else holds or waits on it, so the
/// registry does not grow with every image ever resolved.
class SourceImageLock {
 public:
    ~SourceImageLock();

 private:
    friend class SourceImageCache;
    SourceImageLock(std::shared_ptr<SourceImageCache> cache, const std::string& key,
                    std::shared_ptr<std::mutex> lock);
    SourceImageLock(const SourceImageLock&);
    SourceImageLock& operator=(const SourceImageLock&);

    std::shared_ptr<SourceImageCache> cache_;
    std::string key_;
    std::shared_ptr<std::mutex> lock_;
};

/// Rust `SourceImageCache` — one mutex per source config path, keyed by its
/// stable identity so two spellings of a path share a lock. Async in Rust;
/// here a blocking `std::mutex`, which is what a thread-per-request caller
/// wants.
class SourceImageCache : public std::enable_shared_from_this<SourceImageCache> {
 public:
    /// Rust `acquire_lock`.
    std::shared_ptr<SourceImageLock> AcquireLock(const std::string& image_config_path);

    /// Registry size, for tests (Rust asserts the same through `locks.len()`).
    std::size_t RegisteredLocksForTesting() const;

 private:
    friend class SourceImageLock;
    void Release(const std::string& key, const std::shared_ptr<std::mutex>& lock);

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<std::mutex> > locks_;
};

/// Rust `ImageCacheSourceImageKey` — what identifies a source image in the
/// cache: its manifest digest, plus the repository it came from when known.
class ImageCacheSourceImageKey {
 public:
    /// Rust `new` — the digest must be non-empty after trimming; a blank scope
    /// is no scope.
    static core::Expected<ImageCacheSourceImageKey, std::string> New(
        const std::string& manifest_digest, const core::Optional<std::string>& repository_scope);

    const std::string& manifest_digest() const { return manifest_digest_; }
    const core::Optional<std::string>& repository_scope() const { return repository_scope_; }

 private:
    ImageCacheSourceImageKey() {}
    std::string manifest_digest_;
    core::Optional<std::string> repository_scope_;
};

/// Rust `read_source_image_metadata` — `None` when the sidecar is missing or
/// empty; a sidecar that does not parse is an error.
core::Expected<core::Optional<ImageResolutionMetadata>, std::string> ReadSourceImageMetadata(
    const std::string& metadata_path);

/// Rust `source_image_config_filename` — `<digest-slug>[-<scope>]-image.json`,
/// the scope component bounded to 96 bytes by a hash suffix.
std::string SourceImageConfigFilename(const ImageCacheSourceImageKey& key);

/// Rust `source_image_metadata_path_for_config_path` — `<stem>.metadata.json`
/// next to the config.
std::string SourceImageMetadataPathForConfigPath(const std::string& path);

/// Rust `source_image_config_is_usable` — false (with a warning) when the
/// config is missing or empty, or a local `file=` lower / the upper it names is
/// missing, empty, or of the wrong size.
bool SourceImageConfigIsUsable(const std::string& path);

/// Rust `write_json_atomic` — pretty JSON to `<name>.tmp`, then renamed over
/// `path`, creating the parent first.
core::Expected<core::Unit, std::string> WriteJsonAtomic(const std::string& path,
                                                        const core::Json& value,
                                                        const std::string& label);

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_SOURCE_CONFIG_H_
