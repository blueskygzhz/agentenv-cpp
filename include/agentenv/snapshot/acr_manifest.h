// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/common/acr/manifest.rs
//
// Builds the OCI descriptors, config blob and image manifest that a snapshot
// is published as. The config bytes are canonicalised before hashing, because
// the digest is the image's identity: two runs with the same effective
// configuration must produce byte-identical blobs or every publish would
// create a new layer.
#ifndef AGENTENV_SNAPSHOT_ACR_MANIFEST_H_
#define AGENTENV_SNAPSHOT_ACR_MANIFEST_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/types.h"

namespace agentenv {
namespace snapshot {

/// Rust `OCI_IMAGE_MANIFEST_MEDIA_TYPE` / `OCI_IMAGE_CONFIG_MEDIA_TYPE` /
/// `OCI_TAR_LAYER_MEDIA_TYPE`.
extern const char* const kOciImageManifestMediaType;
extern const char* const kOciImageConfigMediaType;
extern const char* const kOciTarLayerMediaType;

/// Rust `OVERLAYBD_BLOB_DIGEST_ANNOTATION` / `OVERLAYBD_BLOB_SIZE_ANNOTATION` /
/// `SNAPSHOT_TAG_ANNOTATION`.
extern const char* const kOverlaybdBlobDigestAnnotation;
extern const char* const kOverlaybdBlobSizeAnnotation;
extern const char* const kSnapshotTagAnnotation;

/// Rust `struct OciDescriptor`.
struct OciDescriptor {
    std::string media_type;
    std::string digest;
    uint64_t size = 0;
    /// Rust `BTreeMap`, so iteration is key-ordered and the serialised form
    /// is stable; `std::map` gives the same ordering.
    std::map<std::string, std::string> annotations;

    /// Rust `OciDescriptor::overlaybd_layer` — the annotations are what let
    /// the overlaybd snapshotter find the blob behind an ordinary tar layer
    /// media type.
    static OciDescriptor OverlaybdLayer(const std::string& digest, uint64_t size);

    /// Rust `OciDescriptor::config`.
    static OciDescriptor Config(const std::string& digest, uint64_t size);

    core::Json ToJson() const;
    static core::Expected<OciDescriptor, std::string> FromJson(const core::Json& json);

    bool operator==(const OciDescriptor& o) const;
    bool operator!=(const OciDescriptor& o) const { return !(*this == o); }
};

/// Rust `canonicalize_json` — recursively key-sorts every object.
core::Json CanonicalizeJson(const core::Json& value);

/// Rust `merged_runtime_config` — overlays the effective runtime metadata on
/// top of the source image's `config` object.
core::Json MergedRuntimeConfig(const CommandContext& context,
                               const core::Optional<core::Json>& raw_config);

/// Rust `host_architecture_for_oci`.
std::string HostArchitectureForOci();

/// The result of building a config blob: Rust returns `(Vec<u8>, String, u64)`.
struct OciConfigBlob {
    std::string bytes;
    /// `sha256:<hex>` over `bytes`.
    std::string digest;
    uint64_t size = 0;
};

/// Rust `snapshot_oci_config_blob`.
OciConfigBlob SnapshotOciConfigBlob(const std::string& architecture,
                                    const CommandContext& context,
                                    const core::Optional<core::Json>& raw_config);

/// Rust `minimal_oci_config_blob`.
OciConfigBlob MinimalOciConfigBlob(const std::string& architecture);

/// Rust `build_oci_image_manifest`.
std::string BuildOciImageManifest(const OciDescriptor& config,
                                  const std::vector<OciDescriptor>& layers,
                                  const std::string& publication_tag);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_ACR_MANIFEST_H_
