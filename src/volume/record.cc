// SPDX-License-Identifier: MIT
// Rust: src/volume/mod.rs — record/enum/error half.
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
    // `VolumeMode` is a plain `#[derive(Deserialize)]` unit enum upstream, so
    // this stands in for serde's own rejection; word it the way serde does, as
    // the other hand-written variant parsers in the port do.
    return core::make_unexpected("unknown variant `" + raw +
                                 "`, expected `ReadOnly` or `Exclusive`");
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
    // Same reasoning as `VolumeModeParse` above: upstream has no hand-written
    // parser here, so serde's own rejection is what a caller sees, and the
    // text is worth matching because it is the message the catalog surfaces
    // when a record fails to decode. serde's `unknown_variant` switches
    // wording on the variant count -- two variants read "`a` or `b`", three or
    // more read "one of `a`, `b`, `c`" -- which is why this line and the mode's
    // are phrased differently.
    return core::make_unexpected("unknown variant `" + raw +
                                 "`, expected one of `ready`, `uploading`, `failed`");
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
    // against src/volume/mod.rs.
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

// ---- serialisation --------------------------------------------------------

// The key spellings come from `#[serde(rename_all = "camelCase")]` on the Rust
// struct, so the four multi-word fields are `sizeMb`, `reservedBySandboxId`,
// `backingLayers` and `readOnlyMounts` rather than their snake_case field
// names. This is the durable catalog format -- Rust's `encode_record` is a
// plain `serde_json::to_vec_pretty(record)` into `volumes/records/<id>.json`,
// the same path this writes -- so a mismatched key is not a cosmetic
// difference: the field is required, and the other implementation's parse
// fails outright on a record this one wrote.
//
// Note that the *values* follow different rules, and both are deliberate:
// `VolumeMode` carries no `rename_all` and stays PascalCase (`"Exclusive"`),
// while `VolumeStatus` is `rename_all = "lowercase"` (`"ready"`). The nested
// layer refs are snake_case for the same reason as the mode -- no rename on
// `ManagedLayer` / `ExternalLayer`.
//
// What does *not* match is the key order. `serde_json::to_vec_pretty` emits
// fields in declaration order; `core::JsonObject` is a `std::map`, so this
// emits them sorted. That is a port-wide property of `core::Json` rather than
// anything about volumes, and it is compatible in both directions because
// neither parser is order-sensitive -- but it does mean the two
// implementations' bytes for the same record are not identical, so a digest
// of a record document is not portable between them.
core::Json VolumeRecord::ToJson() const {
    core::JsonObject object;
    object["id"] = core::Json(id);
    object["name"] = core::Json(name);
    object["mode"] = core::Json(VolumeModeToString(mode));
    object["sizeMb"] = core::Json(static_cast<int64_t>(size_mb));
    object["status"] = core::Json(VolumeStatusToString(status));
    // Written as an explicit null when unset: there is no
    // `skip_serializing_if` on this field, unlike `deleting` below.
    object["reservedBySandboxId"] = reserved_by_sandbox_id.has_value()
                                        ? core::Json(*reserved_by_sandbox_id)
                                        : core::Json();
    object["backingLayers"] = snapshot::LayerRefsToJson(backing_layers);

    core::JsonArray mounts;
    for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
        mounts.push_back(core::Json(read_only_mounts[i]));
    }
    object["readOnlyMounts"] = core::Json(mounts);

    // `skip_serializing_if = "std::ops::Not::not"`: only written when true.
    if (deleting) object["deleting"] = core::Json(true);
    // `backing_image_config` is `#[serde(skip)]` and deliberately absent.
    return core::Json(object);
}

namespace {

/// serde's `invalid type` wording, so a malformed record reads the same as
/// upstream's parse error.
std::string InvalidType(const char* field, const char* expected) {
    return std::string("invalid type for field `") + field + "`: expected " + expected;
}

core::Expected<std::string, std::string> RequiredString(const core::JsonObject& fields,
                                                        const char* field) {
    const core::JsonObject::const_iterator found = fields.find(field);
    if (found == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (found->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(InvalidType(field, "a string"));
    }
    return found->second.as_string();
}

}  // namespace

core::Expected<VolumeRecord, std::string> VolumeRecord::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct VolumeRecord"));
    }
    const core::JsonObject& fields = json.as_object();
    VolumeRecord record;

    const core::Expected<std::string, std::string> id = RequiredString(fields, "id");
    if (!id.ok()) return core::make_unexpected(id.error());
    record.id = id.value();

    const core::Expected<std::string, std::string> name = RequiredString(fields, "name");
    if (!name.ok()) return core::make_unexpected(name.error());
    record.name = name.value();

    const core::Expected<std::string, std::string> mode = RequiredString(fields, "mode");
    if (!mode.ok()) return core::make_unexpected(mode.error());
    const core::Expected<VolumeMode, std::string> parsed_mode = VolumeModeParse(mode.value());
    if (!parsed_mode.ok()) return core::make_unexpected(parsed_mode.error());
    record.mode = parsed_mode.value();

    const core::JsonObject::const_iterator size_field = fields.find("sizeMb");
    if (size_field == fields.end()) {
        return core::make_unexpected(std::string("missing field `sizeMb`"));
    }
    // Rust's field is `u64`, and `core::Json` has no unsigned kind, so the
    // representable range stops at `i64::MAX` here where serde's stops at
    // `u64::MAX`. The gap is not reachable through upstream: a volume that
    // large cannot be created (`VolumeLimits::max_size_mb` caps it) and
    // `size_mb` is a count of mebibytes, so `i64::MAX` of them is eight
    // zebibytes. A hostile document in that range is refused rather than
    // truncated, which is the safe direction and the only one available
    // without widening `core::Json`.
    if (size_field->second.kind() != core::Json::Kind::Int || size_field->second.as_int() < 0) {
        return core::make_unexpected(InvalidType("sizeMb", "u64"));
    }
    record.size_mb = static_cast<uint64_t>(size_field->second.as_int());

    const core::Expected<std::string, std::string> status = RequiredString(fields, "status");
    if (!status.ok()) return core::make_unexpected(status.error());
    const core::Expected<VolumeStatus, std::string> parsed_status =
        VolumeStatusParse(status.value());
    if (!parsed_status.ok()) return core::make_unexpected(parsed_status.error());
    record.status = parsed_status.value();

    // `Option<String>`: absent and `null` are both `None`.
    const core::JsonObject::const_iterator reserved = fields.find("reservedBySandboxId");
    if (reserved != fields.end() && reserved->second.kind() != core::Json::Kind::Null) {
        if (reserved->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(InvalidType("reservedBySandboxId", "a string"));
        }
        record.reserved_by_sandbox_id = core::Optional<std::string>(reserved->second.as_string());
    }

    const core::JsonObject::const_iterator layers = fields.find("backingLayers");
    if (layers == fields.end()) {
        return core::make_unexpected(std::string("missing field `backingLayers`"));
    }
    const core::Expected<std::vector<snapshot::OverlaybdLayerRef>, std::string> parsed_layers =
        snapshot::LayerRefsFromJson(layers->second);
    if (!parsed_layers.ok()) return core::make_unexpected(parsed_layers.error());
    record.backing_layers = parsed_layers.value();

    const core::JsonObject::const_iterator mounts = fields.find("readOnlyMounts");
    if (mounts == fields.end()) {
        return core::make_unexpected(std::string("missing field `readOnlyMounts`"));
    }
    if (mounts->second.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(InvalidType("readOnlyMounts", "a sequence"));
    }
    const core::JsonArray& values = mounts->second.as_array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected(InvalidType("readOnlyMounts", "a string"));
        }
        record.read_only_mounts.push_back(values[i].as_string());
    }

    // `#[serde(default)]`: absent is `false`.
    const core::JsonObject::const_iterator deleting_field = fields.find("deleting");
    if (deleting_field != fields.end()) {
        if (deleting_field->second.kind() != core::Json::Kind::Bool) {
            return core::make_unexpected(InvalidType("deleting", "a boolean"));
        }
        record.deleting = deleting_field->second.as_bool();
    }

    return record;
}

}  // namespace volume
}  // namespace agentenv
