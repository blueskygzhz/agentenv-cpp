// SPDX-License-Identifier: MIT
// Rust: src/image/{oci_image,local_layer,commit_index,resolver,cache}.rs
#ifndef AGENTENV_IMAGE_LAYERS_H_
#define AGENTENV_IMAGE_LAYERS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/image/metadata.h"
#include "agentenv/image/reference.h"

namespace agentenv {
namespace image {

/// Rust struct `OciDescriptor`.
struct OciDescriptor {
    std::string media_type;
    std::string digest;
    int64_t     size = 0;
};

/// Rust struct `OciManifest`.
struct OciManifest {
    OciDescriptor              config;
    std::vector<OciDescriptor> layers;
};

/// Rust struct `LocalLayer` — a locally-materialized OCI layer.
struct LocalLayer {
    std::string digest;
    std::string local_path;
    int64_t     size = 0;
    bool        is_overlaybd = false;
};

/// Rust struct `CommitIndex` — layer digest -> local path index.
class CommitIndex {
 public:
    virtual ~CommitIndex() {}
    virtual core::Expected<LocalLayer, std::string>
        Lookup(const std::string& digest) const = 0;
    virtual core::Expected<core::Unit, std::string>
        Insert(const LocalLayer& layer) = 0;
};

/// Rust struct `ResolvedBlockImage`.
struct ResolvedBlockImage {
    ImageReference          reference;
    ImageResolutionMetadata meta;
    std::vector<LocalLayer> layers;
    std::string             overlaybd_config_path;
};

/// Rust trait `ImageResolver`.
class ImageResolver {
 public:
    virtual ~ImageResolver() {}
    virtual core::Expected<ResolvedBlockImage, std::string>
        Resolve(const ImageReference& ref) = 0;
};

/// Rust: image cache abstraction (src/image/cache/).
class ImageCache {
 public:
    virtual ~ImageCache() {}
    virtual core::Expected<LocalLayer, std::string>
        FetchLayer(const OciDescriptor& d) = 0;
    virtual core::Expected<core::Unit, std::string>
        Evict(const std::string& digest) = 0;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_LAYERS_H_
