// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/config.rs — CommonConfig / SandboxConfig /
// SnapshotConfig / RuntimePolicy.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_
#define AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/sandbox/network.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/config.rs :: DEFAULT_BOOT_ARGS (with DAMON reclaim tuning).
extern const char* const kDefaultBootArgs;

/// Rust: firecracker/config.rs :: MAX_EXTRA_DRIVES = 'z' - 'c' + 1 = 24.
static const size_t kMaxExtraDrives = static_cast<size_t>('z' - 'c' + 1);

/// Rust: firecracker/config.rs :: CommonConfig.
struct CommonConfig {
    std::string firecracker_bin;
    std::string kernel_image_path;
    std::string kernel_boot_args;
    int32_t     default_vcpus = 1;
    int64_t     default_mem_bytes = 512 * 1024 * 1024;
};

/// A single extra block device, mirroring Rust `ExtraDrive::Overlaybd` fields
/// relevant to validation.
struct ExtraDriveSpec {
    std::string drive_id;
    std::string mount_path;
    std::string image_config_path;
    bool        has_virtual_size = false;
    uint64_t    virtual_size = 0;
};

/// Rust: firecracker/sandbox.rs :: FirecrackerSandboxConfig.
struct SandboxConfig {
    core::SandboxId sandbox_id;
    CommonConfig    common;
    std::string     rootfs_path;
    std::vector<std::string> env_vars;
    network::SandboxNetworkPolicy network_policy;
};

/// Rust: firecracker/config.rs :: FirecrackerSnapshotConfig.
struct SnapshotConfig {
    std::string snapshot_dir;
    bool        enable_diff_snapshots = false;
};

/// Rust: firecracker/config.rs :: FirecrackerRuntimePolicy.
struct RuntimePolicy {
    int32_t boot_timeout_ms = 30000;
    int32_t shutdown_timeout_ms = 10000;
};

/// Rust: firecracker/config.rs :: validate_overlaybd_extra_drive_set —
/// enforces the MAX_EXTRA_DRIVES ceiling, unique drive ids, unique mount paths,
/// non-zero virtual sizes and (optionally) image config existence.
core::Expected<core::Unit, std::string>
    ValidateExtraDriveSet(const std::vector<ExtraDriveSpec>& drives,
                          bool check_image_exists);

/// Rust: firecracker/config.rs :: resolve_serial_output_dir — turns a relative
/// dir into an absolute one (joined onto cwd); empty input yields empty output.
core::Expected<std::string, std::string>
    ResolveSerialOutputDir(const std::string& dir);

/// Rust: firecracker/config.rs :: FirecrackerCommonConfig::validate (host-side
/// portion) — requires the firecracker binary + kernel to exist as files.
core::Expected<core::Unit, std::string>
    ValidateCommonConfig(const CommonConfig& cfg);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_
