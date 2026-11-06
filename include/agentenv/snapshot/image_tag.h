// SPDX-License-Identifier: MIT
// Rust: src/types/image_tag.rs — `ImageName`, `ImageTag`, `DEFAULT_IMAGE_TAG`.
//
// Lives in the snapshot library because Rust's `crate::types` is shared by the
// repository layout (`DocumentKey::ImageTag`) and the image module, and only
// the image library may depend on this one, not the other way round.
#ifndef AGENTENV_SNAPSHOT_IMAGE_TAG_H_
#define AGENTENV_SNAPSHOT_IMAGE_TAG_H_

#include <ostream>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace snapshot {

/// Rust `DEFAULT_IMAGE_TAG` — the tag an image reference without `:tag` means.
///
/// Defined in `image_tag.cc`, alongside the parser that applies it. It used to
/// borrow the definition in `image_export_target.cc`, which happened to link
/// only because both ended up in one library; splitting the record types into
/// their own target broke that.
extern const char* const kDefaultImageTag;

/// Rust `ImageName` — the name of a published image, which owns its tags.
class ImageName {
 public:
    /// Rust `ImageName::parse`: lowercase letters and digits joined by single
    /// `.`, `_` or `-`, at most 128 bytes.
    static core::Expected<ImageName, std::string> Parse(const std::string& name);

    /// Rust `Display` / `AsRef<str>`.
    const std::string& ToString() const { return value_; }

    bool operator==(const ImageName& o) const { return value_ == o.value_; }
    bool operator!=(const ImageName& o) const { return value_ != o.value_; }
    bool operator<(const ImageName& o) const { return value_ < o.value_; }

 private:
    explicit ImageName(const std::string& value) : value_(value) {}
    std::string value_;
};

/// Rust `ImageTag` — a mutable `name:tag` pointer to one image published
/// under its name.
class ImageTag {
 public:
    /// Rust `ImageTag::parse` — `name[:tag]`, the tag defaulting to `latest`.
    /// Split at the first `:`, so `app:v1:extra` is rejected by the tag rule.
    static core::Expected<ImageTag, std::string> Parse(const std::string& input);

    /// Rust `ImageTag::new`: the tag starts with a letter, digit or `_` and
    /// continues with letters, digits, `.`, `_` or `-`, at most 128 bytes.
    static core::Expected<ImageTag, std::string> New(const ImageName& name,
                                                     const std::string& tag);

    const ImageName& name() const { return name_; }
    const std::string& tag() const { return tag_; }

    /// Rust `Display` — `name:tag`.
    std::string ToString() const { return name_.ToString() + ":" + tag_; }

    /// Rust derives `Ord` field by field: name first, then tag.
    bool operator==(const ImageTag& o) const { return name_ == o.name_ && tag_ == o.tag_; }
    bool operator!=(const ImageTag& o) const { return !(*this == o); }
    bool operator<(const ImageTag& o) const {
        return name_ < o.name_ || (name_ == o.name_ && tag_ < o.tag_);
    }

 private:
    ImageTag(const ImageName& name, const std::string& tag) : name_(name), tag_(tag) {}
    ImageName name_;
    std::string tag_;
};

std::ostream& operator<<(std::ostream& os, const ImageName& name);
std::ostream& operator<<(std::ostream& os, const ImageTag& tag);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_TAG_H_
