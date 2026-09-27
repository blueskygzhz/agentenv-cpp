// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — OCI manifest/layer classification (the pure
// logic subset; the regctl/download side effects stay out of this port).
#ifndef AGENTENV_IMAGE_OCI_IMAGE_H_
#define AGENTENV_IMAGE_OCI_IMAGE_H_

#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace image {

/// Rust: overlaybd turbo/version constants.
extern const char* const kOverlaybdTurboArtifactType;
extern const char* const kOverlaybdTurboAnnotationPrefix;
extern const char* const kOverlaybdVersionAnnotation;
extern const char* const kOverlaybdBlobDigestAnnotation;

/// Rust: `OciLayerDescriptor` (classification-relevant subset).
struct OciLayerDescriptor {
    std::string media_type;
    std::string digest;
    int64_t     size = 0;
    std::map<std::string, std::string> annotations;
};

/// Rust: `OciManifest` (classification-relevant subset).
struct OciManifest {
    bool      has_artifact_type = false;
    std::string           artifact_type;
    OciLayerDescriptor              config;
    std::vector<OciLayerDescriptor> layers;
};

/// Rust: `LayerClass`.
enum class LayerClass {
    StandardTar,
    OverlaybdNative,
    OverlaybdTarWrapped,
    OverlaybdTurbo,
    Unknown,
};

/// Rust: `ImageFormat`.
enum class ImageFormat {
    StandardOci,
    OverlaybdNative,
};

/// Rust: `is_overlaybd_native_media_type`.
bool IsOverlaybdNativeMediaType(const std::string& media_type);
/// Rust: `is_standard_tar_media_type`.
bool IsStandardTarMediaType(const std::string& media_type);
/// Rust: `classify_layer`.
LayerClass ClassifyLayer(const OciLayerDescriptor& layer);
/// Rust: `classify_manifest` — rejects empty/mixed/turbo/tar-wrapped/unknown.
core::Expected<ImageFormat, std::string> ClassifyManifest(const OciManifest& manifest);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_OCI_IMAGE_H_
