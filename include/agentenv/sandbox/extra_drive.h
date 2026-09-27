// SPDX-License-Identifier: MIT
// Rust: src/sandbox/extra_drive.rs
#ifndef AGENTENV_SANDBOX_EXTRA_DRIVE_H_
#define AGENTENV_SANDBOX_EXTRA_DRIVE_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {

/// Rust struct `ExtraDrive`.
struct ExtraDrive {
    std::string drive_id;
    std::string mount_path;
    std::string sub_path;
    bool        read_only = false;
    std::string image_ref;
};

/// Rust `validate_drive_id`.
core::Expected<core::Unit, std::string> ValidateDriveId(const std::string& id);
/// Rust `validate_mount_path`.
core::Expected<core::Unit, std::string> ValidateMountPath(const std::string& p);
/// Rust `validate_sub_path`.
core::Expected<core::Unit, std::string> ValidateSubPath(const std::string& p);
/// Rust `normalize_mount_path_for_drive`.
std::string NormalizeMountPathForDrive(const std::string& mount_path);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_EXTRA_DRIVE_H_
