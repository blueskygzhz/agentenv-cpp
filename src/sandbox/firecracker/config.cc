// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{config,manifest,mmds,overlaybd_snapshot}.rs
//
// The data structs live in the headers. This TU ports the pure-logic helpers
// from config.rs: DEFAULT_BOOT_ARGS, extra-drive-set validation, serial output
// dir resolution and the host-side config validation.
#include "agentenv/sandbox/firecracker/config.h"
#include "agentenv/sandbox/firecracker/manifest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <set>

namespace agentenv {
namespace sandbox {
namespace firecracker {

// Rust: DEFAULT_BOOT_ARGS — kept byte-for-byte (DAMON reclaim tuning included).
const char* const kDefaultBootArgs =
    "console=ttyS0 reboot=k panic=1 pci=off "
    "damon_reclaim.enabled=Y "
    "damon_reclaim.min_age=60000000 "
    "damon_reclaim.quota_ms=100 "
    "damon_reclaim.quota_sz=1073741824 "
    "damon_reclaim.quota_reset_interval_ms=1000 "
    "damon_reclaim.wmarks_high=900 "
    "damon_reclaim.wmarks_mid=700 "
    "damon_reclaim.wmarks_low=200 "
    "damon_reclaim.skip_anon=Y "
    "damon_reclaim.wmarks_interval=5000000";

namespace {
bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool path_is_file(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Rust `validate_drive_id`: 1..=63 chars, [A-Za-z0-9_-].
bool valid_drive_id(const std::string& id) {
    if (id.empty() || id.size() > 63) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}
// Rust `validate_mount_path`: must be absolute.
bool valid_mount_path(const std::string& p) {
    return !p.empty() && p[0] == '/';
}
}  // namespace

core::Expected<core::Unit, std::string>
ValidateExtraDriveSet(const std::vector<ExtraDriveSpec>& drives, bool check_image_exists) {
    if (drives.size() > kMaxExtraDrives) {
        return core::make_unexpected(
            std::string("too many extra drives: at most 24 are supported (/dev/vdc..=/dev/vdz)"));
    }
    std::set<std::string> drive_ids;
    std::set<std::string> mount_paths;
    for (size_t i = 0; i < drives.size(); ++i) {
        const ExtraDriveSpec& d = drives[i];
        if (!valid_drive_id(d.drive_id)) {
            return core::make_unexpected(std::string("invalid extra drive id: ") + d.drive_id);
        }
        if (!drive_ids.insert(d.drive_id).second) {
            return core::make_unexpected(std::string("duplicate extra drive id: ") + d.drive_id);
        }
        if (!valid_mount_path(d.mount_path)) {
            return core::make_unexpected(std::string("invalid extra drive mount path: ") + d.mount_path);
        }
        if (!mount_paths.insert(d.mount_path).second) {
            return core::make_unexpected(std::string("duplicate extra drive mount path: ") + d.mount_path);
        }
        if (d.has_virtual_size && d.virtual_size == 0) {
            return core::make_unexpected(std::string("extra drive virtual size must be non-zero: ") + d.drive_id);
        }
        if (check_image_exists && !path_exists(d.image_config_path)) {
            return core::make_unexpected(std::string("overlaybd image config not found at") + d.image_config_path);
        }
    }
    return core::Unit{};
}

core::Expected<std::string, std::string>
ResolveSerialOutputDir(const std::string& dir) {
    if (dir.empty()) return std::string();
    if (dir[0] == '/') return dir;  // already absolute
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
        return core::make_unexpected(std::string("resolve serial output dir: getcwd failed"));
    }
    return std::string(cwd) + "/" + dir;
}

core::Expected<core::Unit, std::string>
ValidateCommonConfig(const CommonConfig& cfg) {
    if (cfg.firecracker_bin.empty() || !path_exists(cfg.firecracker_bin)) {
        return core::make_unexpected(
            std::string("firecracker binary not found at ") + cfg.firecracker_bin);
    }
    if (!cfg.kernel_image_path.empty() && !path_is_file(cfg.kernel_image_path)) {
        return core::make_unexpected(
            std::string("kernel image not found at ") + cfg.kernel_image_path);
    }
    return core::Unit{};
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
