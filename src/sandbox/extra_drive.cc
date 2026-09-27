// SPDX-License-Identifier: MIT
// Rust: src/sandbox/extra_drive.rs
#include "agentenv/sandbox/extra_drive.h"

namespace agentenv {
namespace sandbox {

core::Expected<core::Unit, std::string>
ValidateDriveId(const std::string& id) {
    if (id.empty() || id.size() > 63) {
        return core::make_unexpected(std::string("drive_id length out of range"));
    }
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) {
            return core::make_unexpected(std::string("drive_id has invalid char"));
        }
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
ValidateMountPath(const std::string& p) {
    if (p.empty() || p[0] != '/') {
        return core::make_unexpected(std::string("mount_path must be absolute"));
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
ValidateSubPath(const std::string& p) {
    if (p.find("..") != std::string::npos) {
        return core::make_unexpected(std::string("sub_path must not contain .."));
    }
    return core::Unit{};
}

std::string NormalizeMountPathForDrive(const std::string& mount_path) {
    // Rust: trims trailing slash for canonicalization.
    if (mount_path.size() > 1 && mount_path.back() == '/') {
        return mount_path.substr(0, mount_path.size() - 1);
    }
    return mount_path;
}

}  // namespace sandbox
}  // namespace agentenv
