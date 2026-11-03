// SPDX-License-Identifier: MIT
// Rust: src/types/image_tag.rs
#include "agentenv/snapshot/image_tag.h"

namespace agentenv {
namespace snapshot {

namespace {

const std::size_t kMaxLen = 128;

bool IsLowerOrDigit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool IsAlnum(char c) { return IsLowerOrDigit(c) || (c >= 'A' && c <= 'Z'); }

bool IsNameSeparator(char c) { return c == '.' || c == '_' || c == '-'; }

/// `name.split(['.', '_', '-']).all(|part| !part.is_empty() && ...)` — an
/// empty input is one empty part, so it fails like a doubled separator does.
bool ValidName(const std::string& name) {
    if (name.size() > kMaxLen) return false;
    std::size_t part_len = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (IsNameSeparator(c)) {
            if (part_len == 0) return false;
            part_len = 0;
        } else if (IsLowerOrDigit(c)) {
            ++part_len;
        } else {
            return false;
        }
    }
    return part_len != 0;
}

bool ValidTag(const std::string& tag) {
    if (tag.size() > kMaxLen || tag.empty()) return false;
    if (!(IsAlnum(tag[0]) || tag[0] == '_')) return false;
    for (std::size_t i = 0; i < tag.size(); ++i) {
        const char c = tag[i];
        if (!(IsAlnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

}  // namespace

core::Expected<ImageName, std::string> ImageName::Parse(const std::string& name) {
    if (!ValidName(name)) {
        return core::make_unexpected("invalid image name '" + name +
                                     "': use lowercase letters and digits joined by single "
                                     "'.', '_', or '-'");
    }
    return ImageName(name);
}

core::Expected<ImageTag, std::string> ImageTag::Parse(const std::string& input) {
    const std::size_t colon = input.find(':');
    const std::string name = colon == std::string::npos ? input : input.substr(0, colon);
    const std::string tag =
        colon == std::string::npos ? std::string(kDefaultImageTag) : input.substr(colon + 1);
    const core::Expected<ImageName, std::string> parsed = ImageName::Parse(name);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    return New(parsed.value(), tag);
}

core::Expected<ImageTag, std::string> ImageTag::New(const ImageName& name,
                                                    const std::string& tag) {
    if (!ValidTag(tag)) {
        return core::make_unexpected("invalid image tag '" + tag +
                                     "': use letters, digits, '.', '_', or '-', not starting "
                                     "with '.' or '-'");
    }
    return ImageTag(name, tag);
}

std::ostream& operator<<(std::ostream& os, const ImageName& name) {
    return os << name.ToString();
}

std::ostream& operator<<(std::ostream& os, const ImageTag& tag) { return os << tag.ToString(); }

}  // namespace snapshot
}  // namespace agentenv
