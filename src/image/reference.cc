// SPDX-License-Identifier: MIT
// Rust: src/image/reference.rs
#include "agentenv/image/reference.h"

namespace agentenv {
namespace image {

static bool is_valid_repo_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
           c == '/';
}

std::string ImageReference::ToString() const {
    std::string s;
    if (!registry.empty()) { s += registry; s += "/"; }
    s += repository;
    if (!tag.empty())    { s += ":"; s += tag; }
    if (!digest.empty()) { s += "@"; s += digest; }
    return s;
}

core::Expected<ImageReference, std::string>
ParseReference(const std::string& s) {
    if (s.empty()) {
        return core::make_unexpected(std::string("empty reference"));
    }
    ImageReference r;
    std::string rest = s;

    // 1. split "@digest" if present.
    auto at_pos = rest.find('@');
    if (at_pos != std::string::npos) {
        r.digest = rest.substr(at_pos + 1);
        rest = rest.substr(0, at_pos);
        if (r.digest.find("sha256:") != 0 && r.digest.find("sha512:") != 0) {
            return core::make_unexpected(std::string("unsupported digest algo"));
        }
    }

    // 2. optionally split "registry/" prefix (heuristic: contains '.' or ':' before first '/').
    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        const std::string head = rest.substr(0, slash);
        if (head.find('.') != std::string::npos ||
            head.find(':') != std::string::npos ||
            head == "localhost") {
            r.registry = head;
            rest = rest.substr(slash + 1);
        }
    }

    // 3. split ":tag" if present, but only after the last '/'.
    auto last_slash = rest.rfind('/');
    auto colon = rest.find(':', last_slash == std::string::npos ? 0 : last_slash);
    if (colon != std::string::npos) {
        r.tag = rest.substr(colon + 1);
        rest = rest.substr(0, colon);
    }

    for (char c : rest) {
        if (!is_valid_repo_char(c)) {
            return core::make_unexpected(std::string("invalid character in repo"));
        }
    }
    if (rest.empty()) {
        return core::make_unexpected(std::string("empty repository"));
    }
    r.repository = rest;
    return r;
}

}  // namespace image
}  // namespace agentenv
