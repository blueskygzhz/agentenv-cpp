// SPDX-License-Identifier: MIT
// Rust: `Image` façade in src/image/mod.rs
#ifndef AGENTENV_IMAGE_IMAGE_H_
#define AGENTENV_IMAGE_IMAGE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"
#include "agentenv/image/types.h"

namespace agentenv {
namespace image {

/// A pulled and unpacked image, materialized as a rootfs tree + config.
class Image {
 public:
    Image(Manifest m, ImageConfig cfg, std::string rootfs)
        : manifest_(std::move(m)), config_(std::move(cfg)), rootfs_(std::move(rootfs)) {}

    const Manifest&    manifest() const { return manifest_; }
    const ImageConfig& config()   const { return config_; }
    const std::string& rootfs()   const { return rootfs_; }

 private:
    Manifest    manifest_;
    ImageConfig config_;
    std::string rootfs_;
};

/// Loader — hides "pull from registry" vs "load from local cache".
class ImageLoader {
 public:
    virtual ~ImageLoader() = default;

    /// Reference format: "registry/path:tag" or "sha256:..." digest.
    virtual core::Expected<std::shared_ptr<Image>, core::AnyError>
        Load(const std::string& reference) = 0;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_IMAGE_H_
