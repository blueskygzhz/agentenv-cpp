// SPDX-License-Identifier: MIT
// Rust: src/image/*.rs — OCI image descriptors, manifests, configs.
#ifndef AGENTENV_IMAGE_TYPES_H_
#define AGENTENV_IMAGE_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace image {

/// OCI Content Descriptor (§ image spec v1.1).
struct Descriptor {
    std::string        media_type;
    core::Digest       digest;
    int64_t            size = 0;
    std::vector<std::string> urls;
};

/// OCI Image Manifest (subset).
struct Manifest {
    int schema_version = 2;
    std::string media_type = "application/vnd.oci.image.manifest.v1+json";
    Descriptor config;
    std::vector<Descriptor> layers;
};

/// OCI Image Config (subset).
struct ImageConfig {
    std::string architecture;
    std::string os;
    std::vector<std::string> env;
    std::vector<std::string> entrypoint;
    std::vector<std::string> cmd;
    std::string working_dir;
    std::string user;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_TYPES_H_
