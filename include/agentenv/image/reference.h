// SPDX-License-Identifier: MIT
// Rust: src/image/reference.rs — OCI image reference parser.
#ifndef AGENTENV_IMAGE_REFERENCE_H_
#define AGENTENV_IMAGE_REFERENCE_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace image {

/// Rust struct `ImageReference` — parsed "[registry/]repo[:tag][@digest]".
struct ImageReference {
    std::string registry;   // "docker.io" if empty parsed
    std::string repository; // "library/alpine"
    std::string tag;        // "3.20" or "" if digest-only
    std::string digest;     // "sha256:..." or "" if tag-only

    std::string ToString() const;
};

/// Rust fn `parse_reference`.
core::Expected<ImageReference, std::string>
    ParseReference(const std::string& s);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_REFERENCE_H_
