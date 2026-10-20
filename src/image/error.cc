// SPDX-License-Identifier: MIT
// Rust: src/image/mod.rs — `ImageError` constructors.
#include "agentenv/image/error.h"

namespace agentenv {
namespace image {

namespace {
ImageError Make(ImageError::Kind kind, const std::string& reason) {
    ImageError error;
    error.kind = kind;
    error.reason = reason;
    return error;
}
}  // namespace

ImageError InvalidReference(const std::string& reason) {
    return Make(ImageError::kInvalidReference, reason);
}

ImageError NotFound(const std::string& reason) { return Make(ImageError::kNotFound, reason); }

ImageError UnsupportedImage(const std::string& reason) {
    return Make(ImageError::kUnsupportedImage, reason);
}

ImageError OtherError(const std::string& reason) { return Make(ImageError::kOther, reason); }

}  // namespace image
}  // namespace agentenv
