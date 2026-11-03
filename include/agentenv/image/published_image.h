// SPDX-License-Identifier: MIT
// Rust: src/image/mod.rs — `IMAGE_TAG_PREFIX` and `PublishedImage`.
//
// The description is content-addressed: its digest is the sha256 of the
// compact serde_json encoding with every object's keys in byte order. Rust
// gets that order from `Value::sort_all_objects`; here `JsonObject` is a
// sorted map, so `ToJson().ToString()` already yields the same bytes.
//
// Deserialization follows the derive: `rename_all = "camelCase"`,
// `deny_unknown_fields`, every field required. One gap against serde: a
// duplicated top-level key is collapsed by `core::Json::Parse` (last wins)
// instead of being rejected.
#ifndef AGENTENV_IMAGE_PUBLISHED_IMAGE_H_
#define AGENTENV_IMAGE_PUBLISHED_IMAGE_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/snapshot/layers.h"

namespace agentenv {
namespace image {

/// Rust `IMAGE_TAG_PREFIX`. Short references in this namespace (`aenv/app:v1`)
/// name a published image by tag; a reference with a registry host
/// (`docker.io/aenv/app:v1`) stays an OCI reference.
extern const char* const kImageTagPrefix;

/// Rust `PublishedImage` — the portable description binding ordered
/// repository layers and the OCI runtime configuration. Local paths and build
/// identities are deliberately not part of it, so the digest is the same on
/// every node.
struct PublishedImage {
    uint32_t schema_version;
    std::string os;
    std::string architecture;
    std::vector<snapshot::OverlaybdLayerRef> layers;
    core::Json config;

    PublishedImage() : schema_version(0) {}

    /// Rust `PublishedImage::new` — schema 1, `linux`.
    static PublishedImage New(const std::string& architecture,
                              const std::vector<snapshot::OverlaybdLayerRef>& layers,
                              const core::Json& config);

    /// Rust `serde_json::to_value(self)`.
    core::Json ToJson() const;
    /// Rust `serde_json::from_value::<PublishedImage>`.
    static core::Expected<PublishedImage, std::string> FromJson(const core::Json& value);

    /// Rust `encode` — `(digest, bytes)` after validating.
    core::Expected<std::pair<std::string, std::string>, std::string> Encode() const;
    /// Rust `decode` — checks the digest shape, that `bytes` hash to it, then
    /// parses and validates.
    static core::Expected<PublishedImage, std::string> Decode(const std::string& digest,
                                                              const std::string& bytes);

    /// Rust `validate`.
    core::Expected<core::Unit, std::string> Validate() const;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_PUBLISHED_IMAGE_H_
