// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — classify_layer / classify_manifest.
#include "agentenv/image/oci_image.h"

namespace agentenv {
namespace image {

const char* const kOverlaybdTurboArtifactType =
    "application/vnd.containerd.overlaybd.turbo.v1+json";
const char* const kOverlaybdTurboAnnotationPrefix =
    "containerd.io/snapshot/overlaybd/turbo-oci/";
const char* const kOverlaybdVersionAnnotation =
    "containerd.io/snapshot/overlaybd/version";
const char* const kOverlaybdBlobDigestAnnotation =
    "containerd.io/snapshot/overlaybd/blob-digest";

bool IsOverlaybdNativeMediaType(const std::string& mt) {
    static const char* kTokens[] = {
        ".overlaybd", "/overlaybd", "+overlaybd", ".zfile", "/zfile", "+zfile",
    };
    for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        if (mt.find(kTokens[i]) != std::string::npos) return true;
    }
    return false;
}

bool IsStandardTarMediaType(const std::string& mt) {
    return mt == "application/vnd.oci.image.layer.v1.tar" ||
           mt == "application/vnd.oci.image.layer.v1.tar+gzip" ||
           mt == "application/vnd.oci.image.layer.v1.tar+zstd" ||
      mt == "application/vnd.docker.image.rootfs.diff.tar" ||
     mt == "application/vnd.docker.image.rootfs.diff.tar.gzip";
}

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

LayerClass ClassifyLayer(const OciLayerDescriptor& layer) {
    // Turbo detection wins over everything else.
    bool is_turbo = false;
    for (std::map<std::string, std::string>::const_iterator it = layer.annotations.begin();
         it != layer.annotations.end(); ++it) {
  if (it->first.compare(0, std::string(kOverlaybdTurboAnnotationPrefix).size(),
     kOverlaybdTurboAnnotationPrefix) == 0) {
            is_turbo = true;
 break;
        }
    }
    if (!is_turbo) {
        std::map<std::string, std::string>::const_iterator v =
            layer.annotations.find(kOverlaybdVersionAnnotation);
     if (v != layer.annotations.end() && contains(v->second, "turbo")) {
        is_turbo = true;
        }
    }
    if (is_turbo) return LayerClass::OverlaybdTurbo;

    // Native mediaType wins next.
    if (IsOverlaybdNativeMediaType(layer.media_type)) {
   return LayerClass::OverlaybdNative;
    }
    if (!IsStandardTarMediaType(layer.media_type)) {
 return LayerClass::Unknown;
    }

    // blob-digest annotation: self-referential == native, else tar-wrapped.
    std::map<std::string, std::string>::const_iterator bd =
        layer.annotations.find(kOverlaybdBlobDigestAnnotation);
    if (bd != layer.annotations.end()) {
        if (bd->second == layer.digest) return LayerClass::OverlaybdNative;
        return LayerClass::OverlaybdTarWrapped;
    }
    return LayerClass::StandardTar;
}

core::Expected<ImageFormat, std::string> ClassifyManifest(const OciManifest& manifest) {
    if (manifest.layers.empty()) {
        return core::make_unexpected(std::string("OCI manifest has no layers"));
    }
  if (manifest.has_artifact_type &&
        manifest.artifact_type == kOverlaybdTurboArtifactType) {
        return core::make_unexpected(std::string(
     "image is an overlaybd turbo-OCI image (artifactType); not supported"));
    }

    bool saw_standard = false;
    bool saw_native = false;
    for (size_t i = 0; i < manifest.layers.size(); ++i) {
   LayerClass c = ClassifyLayer(manifest.layers[i]);
        switch (c) {
        case LayerClass::StandardTar:      saw_standard = true; break;
            case LayerClass::OverlaybdNative:  saw_native = true;   break;
            case LayerClass::OverlaybdTurbo:
                return core::make_unexpected(std::string(
         "image is an overlaybd turbo-OCI image (layer); not supported"));
            case LayerClass::OverlaybdTarWrapped:
    return core::make_unexpected(std::string(
          "image has a tar-wrapped overlaybd layer; not supported"));
            case LayerClass::Unknown:
            default:
         return core::make_unexpected(std::string(
   "image layer has an unknown mediaType: ") + manifest.layers[i].media_type);
   }
    }

    if (saw_standard && saw_native) {
    return core::make_unexpected(std::string(
  "image has a mix of standard OCI and overlaybd layers; not supported"));
    }
    if (saw_native) return ImageFormat::OverlaybdNative;
    return ImageFormat::StandardOci;
}

}  // namespace image
}  // namespace agentenv
