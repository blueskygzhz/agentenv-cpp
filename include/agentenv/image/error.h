// SPDX-License-Identifier: MIT
// Rust: src/image/mod.rs — `ImageError` / `ImageResult`.
//
// The image module's single error type. Variants exist only for the
// distinctions a caller actually branches on (the HTTP status it returns);
// every other, server-side failure funnels into `kOther`.
//
// This is not decoration. `src/api/attached_drives.cc` has to answer 400 vs
// 500, and Rust answers it with `err.is_user_error()`. Carrying the failure as
// a bare `std::string` -- which is what this module did before -- forces every
// such site to guess, and the guess that was there turned every bad image
// reference into a 500. A client cannot tell "I typed the tag wrong" from
// "the registry is down" when both come back as a server error.
#ifndef AGENTENV_IMAGE_ERROR_H_
#define AGENTENV_IMAGE_ERROR_H_

#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace image {

/// Rust `enum ImageError`.
///
/// A tagged struct rather than a class hierarchy: callers switch on `kind`,
/// and Rust's own guidance in `mod.rs` is to classify by matching the variant
/// and never by downcasting a type-erased error. The message is always
/// present, because every variant's `Display` is just its reason.
struct ImageError {
    enum Kind {
        /// The reference is syntactically invalid or disallowed by config (400).
        kInvalidReference,
        /// The reference is valid but the registry has no such image/tag (404).
        kNotFound,
        /// The image exists but its format is unsupported -- overlaybd
        /// turbo-OCI, tar-wrapped overlaybd, unknown layer mediaTypes. The
        /// publisher's or caller's problem, so 400 rather than 500.
        kUnsupportedImage,
        /// Any other, server-side failure: network, conversion, storage (500).
        kOther,
    };

    Kind kind;
    std::string reason;

    ImageError() : kind(kOther) {}

    /// Rust `ImageError::is_user_error` — true when the failure is the
    /// caller's fault and should surface as 4xx rather than 5xx.
    bool IsUserError() const {
        return kind == kInvalidReference || kind == kNotFound || kind == kUnsupportedImage;
    }

    /// Rust `ImageError::context` — prepend context while *preserving* the
    /// variant.
    ///
    /// The variant-safe counterpart to `anyhow::Context`, which would collapse
    /// everything into `Other` and so lose the 4xx/5xx classification exactly
    /// when a mid-flight call adds detail. Returns a new error rather than
    /// mutating, matching Rust's by-value signature.
    ImageError Context(const std::string& context) const {
        ImageError out;
        out.kind = kind;
        out.reason = context + ": " + reason;
        return out;
    }

    /// The `Display` of every variant, so a caller that only wants text does
    /// not have to switch.
    const std::string& ToString() const { return reason; }
};

/// Rust `invalid_image_ref` / the `ImageError::*` constructors.
ImageError InvalidReference(const std::string& reason);
ImageError NotFound(const std::string& reason);
ImageError UnsupportedImage(const std::string& reason);
ImageError OtherError(const std::string& reason);

/// Rust `type ImageResult<T> = Result<T, ImageError>`.
template <typename T>
struct ImageResultOf {
    typedef core::Expected<T, ImageError> Type;
};

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_ERROR_H_
