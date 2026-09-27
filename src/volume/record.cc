// SPDX-License-Identifier: MIT
// Rust: src/volume.rs — record/enum/error half.
#include "agentenv/volume/record.h"

#include <sstream>

namespace agentenv {
namespace volume {
namespace {

bool IsAsciiAlphanumeric(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9');
}

/// base64url alphabet, no padding (Rust `URL_SAFE_NO_PAD`).
const char* const kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int Base64UrlDecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

}  // namespace

bool IsValidVolumeComponent(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        if (!IsAsciiAlphanumeric(byte) && byte != '_' && byte != '-') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

const char* VolumeModeToString(VolumeMode mode) {
    switch (mode) {
        case VolumeMode::ReadOnly:
            return "ReadOnly";
        case VolumeMode::Exclusive:
            return "Exclusive";
    }
    return "Exclusive";
}

core::Expected<VolumeMode, std::string> VolumeModeParse(const std::string& raw) {
    if (raw == "ReadOnly") return VolumeMode::ReadOnly;
    if (raw == "Exclusive") return VolumeMode::Exclusive;
    return core::make_unexpected(std::string("unsupported volume mode \"") + raw +
                                 "\"; expected \"ReadOnly\" or \"Exclusive\"");
}

const char* VolumeStatusToString(VolumeStatus status) {
    // Rust `#[serde(rename_all = "lowercase")]`.
    switch (status) {
        case VolumeStatus::Ready:
            return "ready";
        case VolumeStatus::Uploading:
            return "uploading";
        case VolumeStatus::Failed:
            return "failed";
    }
    return "ready";
}

core::Expected<VolumeStatus, std::string> VolumeStatusParse(const std::string& raw) {
    if (raw == "ready") return VolumeStatus::Ready;
    if (raw == "uploading") return VolumeStatus::Uploading;
    if (raw == "failed") return VolumeStatus::Failed;
    return core::make_unexpected(std::string("unsupported volume status \"") + raw + "\"");
}

// ---------------------------------------------------------------------------
// VolumeRecord
// ---------------------------------------------------------------------------

bool VolumeRecord::MountedBy(const std::string& owner) const {
    if (reserved_by_sandbox_id.has_value() && *reserved_by_sandbox_id == owner) return true;
    for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
        if (read_only_mounts[i] == owner) return true;
    }
    return false;
}

bool VolumeRecord::ReplaceOwner(const std::string& from, const core::Optional<std::string>& to) {
    bool changed = false;

    if (reserved_by_sandbox_id.has_value() && *reserved_by_sandbox_id == from) {
        reserved_by_sandbox_id = to;
        changed = true;
    }

    bool holds_read_only = false;
    for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
        if (read_only_mounts[i] == from) {
            holds_read_only = true;
            break;
        }
    }
    if (holds_read_only) {
        // Rust `retain(|owner| owner != from)`.
        std::vector<std::string> kept;
        kept.reserve(read_only_mounts.size());
        for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
            if (read_only_mounts[i] != from) kept.push_back(read_only_mounts[i]);
        }
        read_only_mounts = kept;

        if (to.has_value()) {
            bool already_present = false;
            for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
                if (read_only_mounts[i] == *to) {
                    already_present = true;
                    break;
                }
            }
            if (!already_present) read_only_mounts.push_back(*to);
        }
        changed = true;
    }

    return changed;
}

core::Expected<core::Unit, std::string> VolumeRecord::ValidateCatalogUpdate(
    const VolumeRecord& next) const {
    if (deleting) {
        std::ostringstream oss;
        oss << "volume '" << id << "' is being deleted";
        return core::make_unexpected(oss.str());
    }
    if (name != next.name || mode != next.mode || size_mb != next.size_mb) {
        return core::make_unexpected(std::string("volume identity fields cannot be changed"));
    }
    if (!snapshot::LayerRefsEqual(backing_layers, next.backing_layers) &&
        status != VolumeStatus::Uploading) {
        std::ostringstream oss;
        oss << "volume '" << id << "' must enter uploading state before replacing its backing";
        return core::make_unexpected(oss.str());
    }
    if (status == VolumeStatus::Failed && next.status == VolumeStatus::Ready) {
        std::ostringstream oss;
        oss << "volume '" << id << "' must enter uploading state before becoming ready";
        return core::make_unexpected(oss.str());
    }
    return core::Unit();
}

bool VolumeRecord::operator==(const VolumeRecord& other) const {
    if (id != other.id || name != other.name || mode != other.mode || size_mb != other.size_mb ||
        status != other.status || deleting != other.deleting) {
        return false;
    }
    if (reserved_by_sandbox_id.has_value() != other.reserved_by_sandbox_id.has_value()) {
        return false;
    }
    if (reserved_by_sandbox_id.has_value() &&
        *reserved_by_sandbox_id != *other.reserved_by_sandbox_id) {
        return false;
    }
    // `backing_image_config` is `#[serde(skip)]` but still part of Rust's
    // derived PartialEq, so it is compared here too.
    if (backing_image_config.has_value() != other.backing_image_config.has_value()) return false;
    if (backing_image_config.has_value() &&
        *backing_image_config != *other.backing_image_config) {
        return false;
    }
    if (!snapshot::LayerRefsEqual(backing_layers, other.backing_layers)) return false;
    return read_only_mounts == other.read_only_mounts;
}

// ---------------------------------------------------------------------------
// VolumeError
// ---------------------------------------------------------------------------

namespace {
VolumeError MakeError(VolumeErrorKind kind, std::string detail = std::string()) {
    VolumeError error;
    error.kind = kind;
    error.detail = detail;
    return error;
}
}  // namespace

VolumeError VolumeError::InvalidName() { return MakeError(VolumeErrorKind::InvalidName); }
VolumeError VolumeError::NameConflict(std::string name) {
    return MakeError(VolumeErrorKind::NameConflict, name);
}
VolumeError VolumeError::NotFound(std::string lookup) {
    return MakeError(VolumeErrorKind::NotFound, lookup);
}
VolumeError VolumeError::Reserved(std::string owner) {
    return MakeError(VolumeErrorKind::Reserved, owner);
}
VolumeError VolumeError::Uploading(std::string volume_id) {
    return MakeError(VolumeErrorKind::Uploading, volume_id);
}
VolumeError VolumeError::Failed(std::string volume_id) {
    return MakeError(VolumeErrorKind::Failed, volume_id);
}
VolumeError VolumeError::MultipleSources() {
    return MakeError(VolumeErrorKind::MultipleSources);
}
VolumeError VolumeError::InvalidSize() { return MakeError(VolumeErrorKind::InvalidSize); }
VolumeError VolumeError::SizeLimitExceeded(uint64_t max_size_mb) {
    VolumeError error = MakeError(VolumeErrorKind::SizeLimitExceeded);
    error.max_size_mb = max_size_mb;
    return error;
}
VolumeError VolumeError::SizeMismatch() { return MakeError(VolumeErrorKind::SizeMismatch); }
VolumeError VolumeError::TooManyMountedVolumes(std::size_t max_count) {
    VolumeError error = MakeError(VolumeErrorKind::TooManyMountedVolumes);
    error.max_count = max_count;
    return error;
}
VolumeError VolumeError::SourceNotFound(std::string reference) {
    return MakeError(VolumeErrorKind::SourceNotFound, reference);
}
VolumeError VolumeError::InvalidNextToken() {
    return MakeError(VolumeErrorKind::InvalidNextToken);
}
VolumeError VolumeError::InvalidPageLimit() {
    return MakeError(VolumeErrorKind::InvalidPageLimit);
}
VolumeError VolumeError::Storage(std::string message) {
    return MakeError(VolumeErrorKind::Storage, message);
}

std::string VolumeError::ToString() const {
    std::ostringstream oss;
    switch (kind) {
        case VolumeErrorKind::InvalidName:
            return "volume name must contain only letters, numbers, underscores, or hyphens";
        case VolumeErrorKind::NameConflict:
            oss << "volume name already exists: " << detail;
            return oss.str();
        case VolumeErrorKind::NotFound:
            oss << "volume not found: " << detail;
            return oss.str();
        case VolumeErrorKind::Reserved:
            oss << "volume is reserved by sandbox " << detail;
            return oss.str();
        case VolumeErrorKind::Uploading:
            oss << "volume is uploading and not usable: " << detail;
            return oss.str();
        case VolumeErrorKind::Failed:
            oss << "volume publication failed and is not usable: " << detail;
            return oss.str();
        case VolumeErrorKind::MultipleSources:
            return "fromVolume and image cannot be used together";
        case VolumeErrorKind::InvalidSize:
            return "volume size must be greater than zero";
        case VolumeErrorKind::SizeLimitExceeded:
            oss << "volume size exceeds the configured maximum of " << max_size_mb << " MiB";
            return oss.str();
        case VolumeErrorKind::SizeMismatch:
            return "volume child size must match its source size";
        case VolumeErrorKind::TooManyMountedVolumes:
            oss << "sandbox cannot mount more than " << max_count << " volumes";
            return oss.str();
        case VolumeErrorKind::SourceNotFound:
            oss << "source volume not found: " << detail;
            return oss.str();
        case VolumeErrorKind::InvalidNextToken:
            return "invalid volume next token";
        case VolumeErrorKind::InvalidPageLimit:
            return "volume page limit must be greater than zero";
        case VolumeErrorKind::Storage:
            oss << "volume catalog storage failed: " << detail;
            return oss.str();
    }
    return "unknown volume error";
}

bool VolumeError::operator==(const VolumeError& other) const {
    if (kind != other.kind) return false;
    switch (kind) {
        case VolumeErrorKind::SizeLimitExceeded:
            return max_size_mb == other.max_size_mb;
        case VolumeErrorKind::TooManyMountedVolumes:
            return max_count == other.max_count;
        case VolumeErrorKind::InvalidName:
        case VolumeErrorKind::MultipleSources:
        case VolumeErrorKind::InvalidSize:
        case VolumeErrorKind::SizeMismatch:
        case VolumeErrorKind::InvalidNextToken:
        case VolumeErrorKind::InvalidPageLimit:
            return true;  // unit variants
        default:
            return detail == other.detail;
    }
}

// ---------------------------------------------------------------------------
// Cursor encoding
// ---------------------------------------------------------------------------

std::string EncodeVolumeCursor(const std::string& volume_id) {
    std::string out;
    out.reserve((volume_id.size() + 2) / 3 * 4);

    std::size_t i = 0;
    while (i + 2 < volume_id.size()) {
        const uint32_t triple = (static_cast<unsigned char>(volume_id[i]) << 16) |
                                (static_cast<unsigned char>(volume_id[i + 1]) << 8) |
                                static_cast<unsigned char>(volume_id[i + 2]);
        out.push_back(kBase64UrlAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[triple & 0x3F]);
        i += 3;
    }

    const std::size_t remaining = volume_id.size() - i;
    if (remaining == 1) {
        const uint32_t chunk = static_cast<unsigned char>(volume_id[i]) << 16;
        out.push_back(kBase64UrlAlphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(chunk >> 12) & 0x3F]);
        // No padding: `URL_SAFE_NO_PAD`.
    } else if (remaining == 2) {
        const uint32_t chunk = (static_cast<unsigned char>(volume_id[i]) << 16) |
                               (static_cast<unsigned char>(volume_id[i + 1]) << 8);
        out.push_back(kBase64UrlAlphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(chunk >> 6) & 0x3F]);
    }
    return out;
}

VolumeResult<std::string> DecodeVolumeCursor(const std::string& token) {
    // A `=`-padded or otherwise malformed token is rejected outright, matching
    // `URL_SAFE_NO_PAD.decode`.
    if (token.empty() || token.size() % 4 == 1) {
        return core::make_unexpected(VolumeError::InvalidNextToken());
    }

    std::string decoded;
    decoded.reserve(token.size() / 4 * 3 + 2);

    uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        const int value = Base64UrlDecodeChar(token[i]);
        if (value < 0) return core::make_unexpected(VolumeError::InvalidNextToken());
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero, otherwise the token was not produced by a
    // canonical no-pad encoder.
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return core::make_unexpected(VolumeError::InvalidNextToken());
    }

    // Rust additionally runs `String::from_utf8` and `validate_volume_id`, both
    // mapped to `InvalidNextToken`. `is_valid_volume_component` only admits
    // ASCII, so it subsumes the UTF-8 check.
    if (!IsValidVolumeComponent(decoded)) {
        return core::make_unexpected(VolumeError::InvalidNextToken());
    }
    return decoded;
}

// ---------------------------------------------------------------------------
// Stream operators
// ---------------------------------------------------------------------------

std::ostream& operator<<(std::ostream& os, VolumeMode mode) {
    return os << VolumeModeToString(mode);
}

std::ostream& operator<<(std::ostream& os, VolumeStatus status) {
    return os << VolumeStatusToString(status);
}

std::ostream& operator<<(std::ostream& os, VolumeErrorKind kind) {
    // Names match the Rust variant identifiers so failures are greppable
    // against src/volume.rs.
    switch (kind) {
        case VolumeErrorKind::InvalidName:
            return os << "InvalidName";
        case VolumeErrorKind::NameConflict:
            return os << "NameConflict";
        case VolumeErrorKind::NotFound:
            return os << "NotFound";
        case VolumeErrorKind::Reserved:
            return os << "Reserved";
        case VolumeErrorKind::Uploading:
            return os << "Uploading";
        case VolumeErrorKind::Failed:
            return os << "Failed";
        case VolumeErrorKind::MultipleSources:
            return os << "MultipleSources";
        case VolumeErrorKind::InvalidSize:
            return os << "InvalidSize";
        case VolumeErrorKind::SizeLimitExceeded:
            return os << "SizeLimitExceeded";
        case VolumeErrorKind::SizeMismatch:
            return os << "SizeMismatch";
        case VolumeErrorKind::TooManyMountedVolumes:
            return os << "TooManyMountedVolumes";
        case VolumeErrorKind::SourceNotFound:
            return os << "SourceNotFound";
        case VolumeErrorKind::InvalidNextToken:
            return os << "InvalidNextToken";
        case VolumeErrorKind::InvalidPageLimit:
            return os << "InvalidPageLimit";
        case VolumeErrorKind::Storage:
            return os << "Storage";
    }
    return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, const VolumeError& error) {
    return os << error.kind << "(" << error.ToString() << ")";
}

std::ostream& operator<<(std::ostream& os, const VolumeRecord& record) {
    os << "VolumeRecord{id=" << record.id << ", name=" << record.name << ", mode=" << record.mode
       << ", size_mb=" << record.size_mb << ", status=" << record.status
       << ", layers=" << record.backing_layers.size();
    if (record.reserved_by_sandbox_id.has_value()) {
        os << ", reserved_by=" << *record.reserved_by_sandbox_id;
    }
    if (!record.read_only_mounts.empty()) {
        os << ", read_only_mounts=" << record.read_only_mounts.size();
    }
    if (record.deleting) os << ", deleting";
    return os << "}";
}

core::Expected<core::Unit, VolumeError> ValidateName(const std::string& name) {
    if (IsValidVolumeComponent(name)) return core::Unit();
    return core::make_unexpected(VolumeError::InvalidName());
}

core::Expected<core::Unit, VolumeError> ValidateVolumeId(const std::string& id) {
    if (IsValidVolumeComponent(id)) return core::Unit();
    std::ostringstream oss;
    oss << "invalid volume id '" << id << "'";
    return core::make_unexpected(VolumeError::Storage(oss.str()));
}

}  // namespace volume
}  // namespace agentenv
