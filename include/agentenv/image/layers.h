// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — `ResolvedBlockImage` and the resolver seam.
//
// This header used to carry five more types, all of them invented here with a
// "Rust:" attribution that pointed at nothing:
//
//  * `OciDescriptor` / `OciManifest` — `oci_image.h` already declares an
//    `OciManifest`, with different members, in this same namespace. Two
//    definitions of one name is ill-formed the moment both headers are
//    included; it only ever compiled because nothing included both.
//  * `CommitIndex` (a `Lookup`/`Insert` interface) — collides the same way
//    with `commit_index.h`'s `CommitIndex`, which is the real thing: a sidecar
//    record, not a lookup table.
//  * `ImageCache` (`FetchLayer`/`Evict`) — Rust's cache layer is four
//    separate traits in `cache/store.rs` with no such methods.
//
// They are gone rather than renamed: an invented interface with no
// implementation is not a placeholder, it is a claim that the shape has been
// decided, and these shapes were decided wrong.
#ifndef AGENTENV_IMAGE_LAYERS_H_
#define AGENTENV_IMAGE_LAYERS_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/error.h"
#include "agentenv/image/local_layer.h"
#include "agentenv/image/metadata.h"

namespace agentenv {
namespace image {

/// Rust struct `ResolvedBlockImage`.
struct ResolvedBlockImage {
    /// The fully-qualified reference that actually resolved — not what the
    /// caller typed. A short name expands to several candidates and only one
    /// of them wins, so the caller cannot reconstruct this.
    std::string image_ref;
    std::string overlaybd_config_path;
    ImageBaseContext base_context;
    /// Rust `raw_config: Option<Value>` — absent when the source has no config
    /// at all (a bare overlaybd config path) or when the entry came from a
    /// legacy cache record written before this field existed. Absent is
    /// therefore normal and must not be treated as a resolution failure.
    core::Optional<core::Json> raw_config;

    bool operator==(const ResolvedBlockImage& other) const;
    bool operator!=(const ResolvedBlockImage& other) const { return !(*this == other); }
};

/// Rust `ImageResolver`, reduced to the seam its consumers need.
///
/// Upstream this is a concrete struct with eleven config fields and a `resolve`
/// that drives regctl, BuildKit and the image cache. None of that is ported
/// yet, so what stands here is the call shape only — and deliberately the
/// *correct* one:
///
///  * `Resolve` takes the raw reference string. Rust calls
///    `resolver.resolve(&drive.image)` directly; there is no parse step, and
///    the one this port used to perform decomposed the string with rules Rust
///    does not have.
///  * It fails with `ImageError`, not a string, so a caller can still answer
///    400 versus 500 the way `is_user_error` does upstream.
class ImageResolver {
 public:
    virtual ~ImageResolver() {}

    /// Rust `ImageResolver::resolve`.
    virtual core::Expected<ResolvedBlockImage, ImageError> Resolve(
        const std::string& image_ref) = 0;

    /// Rust `ImageResolver::default_image` — the reference used when a request
    /// names none.
    virtual const std::string& DefaultImage() const = 0;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_LAYERS_H_
