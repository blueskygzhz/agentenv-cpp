// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/publisher.rs — the tag layer.
#include "agentenv/snapshot/image_export/acr_publisher.h"

#include "agentenv/core/digest.h"
#include "agentenv/snapshot/record.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

using repository::RepositoryError;
using repository::RepositoryResult;

namespace {

/// Rust `is_tag_start` — `char::is_ascii_alphanumeric() || ch == '_'`.
///
/// `-` is excluded deliberately: a leading `-` would be read as a flag by the
/// registry tooling a tag is pasted into.
bool IsTagStart(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/// Rust `is_tag_char`.
bool IsTagChar(unsigned char ch) {
    return IsTagStart(ch) || ch == '.' || ch == '-';
}

/// The byte length of the UTF-8 sequence starting at `first`.
///
/// Needed because Rust maps one `char` to one `-`. Iterating bytes instead
/// would turn a single three-byte character into three `-`, so a drive id
/// written in CJK would produce a different tag than upstream for the same
/// input — and the tag is what the published manifest is addressed by.
///
/// A continuation or otherwise invalid lead byte counts as one byte. Rust
/// cannot reach that case (`&str` is guaranteed UTF-8), and advancing by one
/// keeps this terminating on a `std::string` that is not.
std::size_t Utf8SequenceLength(unsigned char first) {
    if (first < 0x80) return 1;
    if (first >= 0xF0 && first <= 0xF7) return 4;
    if (first >= 0xE0 && first <= 0xEF) return 3;
    if (first >= 0xC0 && first <= 0xDF) return 2;
    return 1;
}

RepositoryResult<core::Unit> InvalidTag(const std::string& tag) {
    return core::make_unexpected(RepositoryError::InvalidRequest(
        "snapshot ACR tag '" + tag + "' is not a valid OCI tag"));
}

}  // namespace

std::string SanitizeDriveTagComponent(const std::string& drive_id) {
    // Hashed before sanitising, so two ids that sanitise or truncate onto the
    // same text still produce different tags.
    const std::string hash = core::Sha256Hex(drive_id);

    std::string sanitized;
    sanitized.reserve(drive_id.size());
    for (std::size_t at = 0; at < drive_id.size();) {
        const unsigned char first = static_cast<unsigned char>(drive_id[at]);
        const std::size_t width = Utf8SequenceLength(first);
        // One `char` in, one character out — a multi-byte sequence collapses
        // to a single `-`, as Rust's `chars()` does.
        if (width == 1 && IsTagChar(first)) {
            sanitized += static_cast<char>(first);
        } else {
            sanitized += '-';
        }
        at += width;
    }

    // Reachable only for an empty id, since every character maps to one
    // character. Checked before truncating, matching upstream's order.
    if (sanitized.empty()) sanitized = "drive";
    if (sanitized.size() > 32) sanitized.resize(32);

    // `Sha256Hex` is lowercase hex, so this is always 64 characters and the
    // prefix is never short.
    return sanitized + "-" + hash.substr(0, 12);
}

std::string SnapshotTag(const core::SnapshotId& snapshot_id,
                        const DiskImageSubject& subject) {
    const std::string rootfs_tag = RootfsSnapshotImageTag(snapshot_id);
    if (subject.is_rootfs()) return rootfs_tag;
    return rootfs_tag + "-drive-" + SanitizeDriveTagComponent(subject.drive_id);
}

RepositoryResult<core::Unit> ValidateSnapshotTag(const std::string& tag) {
    if (tag.empty()) return InvalidTag(tag);
    // Rust compares `str::len()`, which is bytes. Equivalent here: any
    // non-ASCII byte fails `IsTagChar` below, so a tag that passes is all
    // ASCII and its byte length is its character count.
    if (tag.size() > 128) return InvalidTag(tag);
    if (!IsTagStart(static_cast<unsigned char>(tag[0]))) return InvalidTag(tag);
    for (std::size_t i = 1; i < tag.size(); ++i) {
        if (!IsTagChar(static_cast<unsigned char>(tag[i]))) return InvalidTag(tag);
    }
    return core::Unit();
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
