// SPDX-License-Identifier: MIT
// Rust: src/sandbox/extra_drive.rs
#include "agentenv/sandbox/extra_drive.h"

#include <cctype>

namespace agentenv {
namespace sandbox {

const char* const kDefaultExtraDriveMountRoot = "/mnt";
const char* const kRootfsDriveId = "rootfs";
const char* const kUserRootfsDriveId = "user_rootfs";
const char* const kVolumeDriveSlotPrefix = "agentenv_volume_slot_";

namespace {

/// Splits a path the way Rust's `Path::components` does for the checks here:
/// yields only `Normal` components, dropping the root, empty segments from
/// repeated separators, and `.`. `..` is reported separately because every
/// caller rejects it rather than resolving it — resolving would let a path
/// that looks safe after normalisation have escaped a reserved prefix on the
/// way.
void SplitComponents(const std::string& path, std::vector<std::string>* normal,
                     bool* has_parent_dir) {
    normal->clear();
    *has_parent_dir = false;

    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        std::size_t begin = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i == begin) break;
        const std::string segment = path.substr(begin, i - begin);
        if (segment == ".") continue;
        if (segment == "..") {
            *has_parent_dir = true;
            continue;
        }
        normal->push_back(segment);
    }
}

/// Rust `Path::starts_with` — component-wise, so `/p` is not a prefix of
/// `/proc`. A plain string prefix test here would let `/proc-data` collide
/// with the reserved `/proc`.
bool PathStartsWith(const std::vector<std::string>& path,
                    const std::vector<std::string>& prefix) {
    if (prefix.size() > path.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (path[i] != prefix[i]) return false;
    }
    return true;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

/// Rust: `raw.chars().any(char::is_whitespace) || raw.contains(',') ||
/// raw.contains(':')`. Commas and colons are separators in the mount options
/// string these paths are spliced into, so allowing them would let one drive
/// inject an extra option.
bool HasForbiddenCharacters(const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (std::isspace(c) != 0 || c == ',' || c == ':') return true;
    }
    return false;
}

std::string JoinNormal(const std::vector<std::string>& components) {
    std::string joined = "/";
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (joined.size() > 1) joined += "/";
        joined += components[i];
    }
    return joined;
}

}  // namespace

// ---- validation -----------------------------------------------------------

core::Expected<core::Unit, std::string> ValidateDriveId(const std::string& drive_id) {
    if (Trim(drive_id).empty()) {
        return core::make_unexpected(std::string("attached drive driveID must not be empty"));
    }
    for (std::size_t i = 0; i < drive_id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(drive_id[i]);
        // Rust: `is_ascii_alphanumeric() || byte == b'_'`. No dashes: the id
        // becomes part of a Firecracker drive id and a filesystem path.
        const bool allowed = std::isalnum(c) != 0 && c < 0x80;
        if (!allowed && c != '_') {
            return core::make_unexpected(
                std::string("attached drive driveID must contain only ASCII letters, numbers,"
                            " and underscores: ") +
                drive_id);
        }
    }
    // Reserved ids would collide with the drives AgentENV attaches itself.
    if (drive_id == kRootfsDriveId || drive_id == kUserRootfsDriveId ||
        StartsWith(drive_id, kVolumeDriveSlotPrefix)) {
        return core::make_unexpected(std::string("attached drive driveID is reserved: ") +
                                     drive_id);
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> ValidateMountPath(const std::string& path) {
    if (path.empty() || path[0] != '/') {
        return core::make_unexpected(
            std::string("attached drive mountPath must be absolute: ") + path);
    }
    if (path == "/") {
        return core::make_unexpected(std::string("attached drive mountPath must not be /"));
    }
    if (HasForbiddenCharacters(path)) {
        return core::make_unexpected(
            std::string("attached drive mountPath must not contain whitespace, commas, or"
                        " colons: ") +
            path);
    }

    std::vector<std::string> components;
    bool has_parent_dir = false;
    SplitComponents(path, &components, &has_parent_dir);
    if (has_parent_dir) {
        return core::make_unexpected(
            std::string("attached drive mountPath must not contain '..': ") + path);
    }

    // Rust `RESERVED`. `/tmp` is deliberately absent: it belongs to the guest
    // and replacing it hides nothing from the control plane.
    static const char* const kReserved[] = {
        "/proc", "/sys", "/dev", "/run", "/agentenv", "/opt/agentenv",
    };
    for (std::size_t i = 0; i < sizeof(kReserved) / sizeof(kReserved[0]); ++i) {
        std::vector<std::string> reserved_components;
        bool reserved_parent = false;
        SplitComponents(kReserved[i], &reserved_components, &reserved_parent);

        // Both directions: a descendant of a reserved path would overlay it,
        // and an ancestor would shadow it (mounting `/opt` hides
        // `/opt/agentenv`).
        if (PathStartsWith(components, reserved_components) ||
            PathStartsWith(reserved_components, components)) {
            return core::make_unexpected(
                std::string("attached drive mountPath conflicts with reserved path ") +
                kReserved[i] + ": " + path);
        }
    }
    return core::Unit();
}

core::Expected<std::string, std::string> ValidateSubPath(const std::string& sub_path) {
    if (sub_path.empty()) {
        return core::make_unexpected(std::string("attached drive subPath must not be empty"));
    }
    if (sub_path[0] == '/') {
        return core::make_unexpected(
            std::string("attached drive subPath must be a relative path: ") + sub_path);
    }
    if (HasForbiddenCharacters(sub_path)) {
        return core::make_unexpected(
            std::string("attached drive subPath must not contain whitespace, commas, or"
                        " colons: ") +
            sub_path);
    }

    std::vector<std::string> components;
    bool has_parent_dir = false;
    SplitComponents(sub_path, &components, &has_parent_dir);
    if (has_parent_dir) {
        // A `..` here would bind a directory from outside the drive root.
        return core::make_unexpected(
            std::string("attached drive subPath must not contain '..': ") + sub_path);
    }
    return sub_path;
}

core::Expected<std::string, std::string> NormalizeMountPath(const std::string& mount_path) {
    const core::Expected<core::Unit, std::string> validated = ValidateMountPath(mount_path);
    if (!validated.ok()) return core::make_unexpected(validated.error());

    std::vector<std::string> components;
    bool has_parent_dir = false;
    SplitComponents(mount_path, &components, &has_parent_dir);
    return JoinNormal(components);
}

core::Expected<std::string, std::string> NormalizeMountPathForDrive(
    const std::string& drive_id, const std::string& mount_path) {
    // An empty mount path is how the API says "use the default", so it is
    // substituted before validation rather than rejected by it.
    const std::string effective =
        mount_path.empty() ? ExtraDrive::DefaultMountPath(drive_id) : mount_path;
    return NormalizeMountPath(effective);
}

// ---- ExtraDrive -----------------------------------------------------------

std::string ExtraDrive::DefaultMountPath(const std::string& drive_id) {
    return std::string(kDefaultExtraDriveMountRoot) + "/" + drive_id;
}

core::Expected<ExtraDrive, std::string> ExtraDrive::TryNewOverlaybdWithMountPath(
    const std::string& drive_id, const std::string& image_config_path, bool read_only,
    const std::string& mount_path, const core::Optional<std::string>& sub_path) {
    const core::Expected<core::Unit, std::string> id_valid = ValidateDriveId(drive_id);
    if (!id_valid.ok()) return core::make_unexpected(id_valid.error());

    const core::Expected<std::string, std::string> normalized =
        NormalizeMountPathForDrive(drive_id, mount_path);
    if (!normalized.ok()) return core::make_unexpected(normalized.error());

    ExtraDrive drive;
    drive.kind = Kind::Overlaybd;
    drive.drive_id = drive_id;
    drive.image_config_path = image_config_path;
    drive.read_only = read_only;
    drive.mount_path = normalized.value();
    drive.volume = false;

    if (sub_path.has_value()) {
        const core::Expected<std::string, std::string> validated = ValidateSubPath(*sub_path);
        if (!validated.ok()) return core::make_unexpected(validated.error());
        drive.sub_path = core::Optional<std::string>(validated.value());
    }
    return drive;
}

core::Expected<ExtraDrive, std::string> ExtraDrive::TryNewOverlaybd(
    const std::string& drive_id, const std::string& image_config_path, bool read_only) {
    return TryNewOverlaybdWithMountPath(drive_id, image_config_path, read_only,
                                        DefaultMountPath(drive_id),
                                        core::Optional<std::string>());
}

std::string ExtraDrive::RuntimeDir(const std::string& sandbox_work_dir) const {
    return sandbox_work_dir + "/extra-drive-runtime-" + drive_id;
}

std::string ExtraDrive::AttachmentSymlinkName() const {
    return std::string("extra-drive-") + drive_id;
}

ExtraDrive ExtraDrive::WithVolumeSnapshotOutputDir(
    const core::Optional<std::string>& output_dir) const {
    ExtraDrive copy = *this;
    copy.snapshot_output_dir = output_dir;
    // Rust sets `volume: true` unconditionally in this constructor.
    copy.volume = true;
    return copy;
}

ExtraDrive ExtraDrive::WithImageConfigPath(const std::string& path) const {
    ExtraDrive copy = *this;
    copy.image_config_path = path;
    return copy;
}

core::Expected<ExtraDrive, std::string> ExtraDrive::TryWithVirtualSize(uint64_t size) const {
    if (size == 0) {
        return core::make_unexpected(std::string("extra drive virtual size must be non-zero"));
    }
    ExtraDrive copy = *this;
    copy.virtual_size = core::Optional<uint64_t>(size);
    return copy;
}

bool ExtraDrive::operator==(const ExtraDrive& o) const {
    if (kind != o.kind || drive_id != o.drive_id ||
        image_config_path != o.image_config_path || read_only != o.read_only ||
        mount_path != o.mount_path || volume != o.volume) {
        return false;
    }
    if (virtual_size.has_value() != o.virtual_size.has_value()) return false;
    if (virtual_size.has_value() && *virtual_size != *o.virtual_size) return false;
    if (sub_path.has_value() != o.sub_path.has_value()) return false;
    if (sub_path.has_value() && *sub_path != *o.sub_path) return false;
    if (snapshot_output_dir.has_value() != o.snapshot_output_dir.has_value()) return false;
    if (snapshot_output_dir.has_value() && *snapshot_output_dir != *o.snapshot_output_dir) {
        return false;
    }
    return true;
}

// ---- ExtraDrivePrepareMode ------------------------------------------------

ExtraDrivePrepareMode ExtraDrivePrepareMode::MakeFresh(bool allow_shrink) {
    ExtraDrivePrepareMode mode;
    mode.kind = Kind::Fresh;
    mode.allow_shrink = allow_shrink;
    return mode;
}

ExtraDrivePrepareMode ExtraDrivePrepareMode::MakeResume() {
    ExtraDrivePrepareMode mode;
    mode.kind = Kind::Resume;
    mode.allow_shrink = false;
    return mode;
}

void ExtraDrivePrepareMode::DeviceSizes(const ExtraDrive& drive,
                                        core::Optional<uint64_t>* target_size,
                                        core::Optional<uint64_t>* base_size) const {
    if (kind == Kind::Fresh) {
        // The requested target size is known; the base size is deliberately
        // left unset so the daemon reads it from the source image.
        *target_size = drive.virtual_size;
        *base_size = core::Optional<uint64_t>();
        return;
    }
    // On resume `virtual_size` is the recorded actual device size, so it is
    // both the target and the base.
    *target_size = drive.virtual_size;
    *base_size = drive.virtual_size;
}

bool ExtraDrivePrepareMode::AllowShrink() const {
    return kind == Kind::Fresh ? allow_shrink : false;
}

namespace {

/// Splits an absolute path into components, dropping empty ones so a doubled
/// separator does not create a phantom component.
std::vector<std::string> MountPathComponents(const std::string& path) {
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string component =
            end == std::string::npos ? path.substr(begin) : path.substr(begin, end - begin);
        if (!component.empty()) components.push_back(component);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return components;
}

/// Rust `Path::starts_with` — component-wise, not a raw string prefix.
bool ComponentsStartWith(const std::vector<std::string>& path,
                         const std::vector<std::string>& prefix) {
    if (prefix.size() > path.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (path[i] != prefix[i]) return false;
    }
    return true;
}

}  // namespace

bool MountPathsOverlap(const std::string& a, const std::string& b) {
    const std::vector<std::string> left  = MountPathComponents(a);
    const std::vector<std::string> right = MountPathComponents(b);
    // Either being a component-wise prefix of the other means one would shadow
    // the other inside the guest.
    return ComponentsStartWith(left, right) || ComponentsStartWith(right, left);
}

}  // namespace sandbox
}  // namespace agentenv
