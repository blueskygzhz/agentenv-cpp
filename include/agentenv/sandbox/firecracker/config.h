// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/config.rs — FirecrackerRuntimePolicy /
// FirecrackerCommonConfig / FirecrackerSandboxConfig / FirecrackerSnapshotConfig.
//
// These are the configs that describe a microVM *before* anything is launched,
// which is why the whole file is portable: the validation here decides whether
// a boot or a resume is attempted at all.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_
#define AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/network/policy.h"
#include "agentenv/sandbox/ublk.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust: firecracker/config.rs :: DEFAULT_BOOT_ARGS — the fallback when
/// `config.firecracker.boot_args` is unset. Must stay in sync with
/// `config/default.toml`.
///
/// DAMON reclaim parameters:
///   min_age        = 100 000 us (100 ms) — page must be cold this long first
///   quota_ms       = 20            — at most 20 ms per interval reclaiming
///   quota_sz       = 1 GiB         — at most 1 GiB per interval
///   quota_reset_interval_ms = 500  — reset quota counters every 500 ms
///   wmarks_high    = 990 (permille) — stop reclaim when free pages > 99 %
///   wmarks_mid     = 990 (permille) — start reclaim when free pages < 99 %
///   wmarks_low     = 200 (permille) — below 20 % stop DAMON, fall back to LRU
///   wmarks_interval= 1 000 000 us  — check watermarks every 1 s
///   skip_anon      = Y             — only reclaim file-backed (pagecache) pages
extern const char* const kDefaultBootArgs;

/// Rust: firecracker/config.rs :: MAX_EXTRA_DRIVES = 'z' - 'c' + 1 = 24.
static const std::size_t kMaxExtraDrives = static_cast<std::size_t>('z' - 'c' + 1);

/// Rust: firecracker/config.rs :: FirecrackerRuntimePolicy.
///
/// Rust carries `tokio::time::Duration`; the units are made explicit in the
/// field names here so a millisecond value cannot be read as seconds.
struct RuntimePolicy {
    uint64_t socket_timeout_ms = 3000;
    uint64_t socket_poll_interval_ms = 1;
    uint64_t envd_timeout_ms = 60000;
    uint64_t envd_poll_interval_ms = 3;

    /// Rust `FirecrackerRuntimePolicy::from_app_config`.
    static RuntimePolicy FromAppConfig(const cfg::AppConfig& config);
};

/// Rust: firecracker/config.rs :: FirecrackerCommonConfig — everything a fresh
/// boot and a snapshot resume have in common.
///
/// Rust marks several fields `#[serde(skip)]`; those are runtime-only and are
/// flagged individually below, because whether a field survives into a
/// persisted snapshot config is part of the contract, not an encoding detail.
struct CommonConfig {
    std::string firecracker_binary;

    /// Immutable release version of the complete tools drive. Empty means the
    /// sandbox state predates version recording and cannot be resumed.
    std::string tools_drive_version;

    /// Optional parent dir for this sandbox's host-side runtime working dir
    /// (sockets/symlinks/logs, overlaybd writable upper files). Unrelated to
    /// the guest's `default_workdir`. Empty means the system temp dir.
    core::Optional<std::string> firecracker_work_base_dir;

    /// Base dir for serial output; files land in `{base}/{sandbox_id}/`. Used
    /// only when logging is enabled, and overridden by explicit stdout/stderr.
    core::Optional<std::string> serial_output_base_dir;
    /// Explicit stdout destination; enables capture even with no log level.
    core::Optional<std::string> stdout_path;
    /// Explicit stderr destination; enables capture even with no log level.
    core::Optional<std::string> stderr_path;
    /// A non-empty level enables stdout/stderr capture plus Firecracker
    /// logging to `firecracker.log`. Unset/empty disables both unless an
    /// explicit stdout/stderr destination is given.
    core::Optional<std::string> firecracker_log_level;

    RuntimePolicy runtime_policy;

    /// KVM dirty-page tracking, needed for memory snapshot capture.
    bool track_dirty_pages = true;

    std::string envd_version;
    /// Control plane port inside the VM.
    uint16_t control_plane_port = 49983;

    /// Rust `Option<HashMap<String, String>>`: absent and empty are distinct —
    /// `apply_launch_config` only creates the map when there is something to
    /// put in it.
    core::Optional<std::vector<std::pair<std::string, std::string> > > env_vars;
    core::Optional<std::string> default_workdir;
    core::Optional<std::string> default_user;

    /// Rust `mmds_metadata: Option<MmdsMetadata>` — the rendered MMDS document.
    /// Held as its serialized JSON so this header does not pull in mmds.h; the
    /// document is only ever handed to the API socket as a body anyway.
    core::Optional<std::string> mmds_metadata_json;

    /// OverlayBD config for the sandbox user rootfs drive.
    core::Optional<ublk::OverlaybdDeviceConfig> rootfs_image_config;
    /// Virtual size of the user rootfs block device, in bytes.
    core::Optional<uint64_t> rootfs_virtual_size;
    bool rootfs_allow_shrink = false;

    std::vector<ExtraDrive> extra_drives;

    /// Extra drives physically present in the snapshot, before its reserved
    /// launch-time volume slots.
    std::size_t physical_extra_drive_count = 0;
    /// Existing placeholder drives that can be rebound to launch-time volumes.
    std::size_t volume_drive_slots = 0;

    core::Optional<ublk::UblkConfig> ublk_config;

    /// Cluster-wide CPU intersection from the scheduler. Applied via
    /// `PUT /cpu-config` before first boot only.
    core::Optional<std::string> cpu_config_json;

    core::Optional<network::SandboxNetworkPolicy> network_policy;

    /// Opaque user JSON forwarded to the custom extension start hooks.
    core::Optional<std::string> custom_extension_params_json;

    /// Effective disk I/O rate limit for the user rootfs drive. Carried here
    /// rather than read from the process-global config at each site, so fresh
    /// boot and resume apply the same limit and it survives pause/resume.
    cfg::DiskRateLimitConfig disk_rate_limit;

    /// Rust `#[serde(skip)] envd_access_token` — runtime-only. Re-derived from
    /// sandbox metadata on resume and deliberately absent from persisted
    /// snapshot configs, so a leaked snapshot cannot carry a live credential.
    core::Optional<std::string> envd_access_token;

    /// Rust `FirecrackerCommonConfig::new`.
    static CommonConfig New(const std::string& firecracker_binary,
                            const std::string& tools_drive_version,
                            const RuntimePolicy& runtime_policy);

    /// Rust `FirecrackerCommonConfig::from_app_config`.
    static core::Expected<CommonConfig, std::string> FromAppConfig(
        const cfg::AppConfig& config);

    /// Rust `FirecrackerCommonConfig::validate` — the launch-time check. Needs
    /// a Linux host and an existing binary on top of the persisted-artifact
    /// checks.
    core::Expected<core::Unit, std::string> Validate() const;

    /// Rust `FirecrackerCommonConfig::validate_persisted_artifacts` — the
    /// subset that only inspects recorded artifacts, so a stored config can be
    /// checked on a node that is not about to launch it.
    core::Expected<core::Unit, std::string> ValidatePersistedArtifacts() const;

    /// Rust `FirecrackerCommonConfig::resolved_tools_drive_path`.
    ///
    /// Refuses an empty version rather than falling back to a default: a
    /// sandbox whose metadata predates version recording must be migrated, not
    /// resumed against whichever tools drive happens to be current.
    core::Expected<std::string, std::string> ResolvedToolsDrivePath(
        const cfg::AppConfig& config) const;
};

/// Rust: firecracker/config.rs :: logging_enabled lives in `lifecycle.h` —
/// it is declared once there rather than in both headers, so the "blank level
/// means off" rule cannot be restated two different ways.

/// Rust: firecracker/config.rs :: FirecrackerSandboxConfig — a fresh boot.
struct SandboxConfig {
    CommonConfig common;
    std::string kernel_image;
    core::Optional<std::string> boot_args;
    uint32_t vcpu_count = 2;
    uint32_t mem_size_mib = 1024;

    /// Rust `FirecrackerSandboxConfig::new`.
    static SandboxConfig New(const std::string& firecracker_binary,
                             const std::string& kernel_image,
                             const std::string& tools_drive_version,
                             const std::string& user_image_config_path);

    /// Rust `FirecrackerSandboxConfig::validate`.
    core::Expected<core::Unit, std::string> Validate() const;
};

/// The parts of a `RunnableSnapshot` that [`SnapshotConfigFromRunnable`] reads.
///
/// Rust takes `&RunnableSnapshot` directly. Here the library dependency runs
/// snapshot → sandbox, so this layer cannot name that type; the caller unpacks
/// it instead. Listing the inputs explicitly also documents exactly which parts
/// of a committed record take part in the projection.
struct RunnableSnapshotView {
    /// Only for error messages, which name the snapshot being refused.
    std::string snapshot_id;
    /// Rust `snapshot.committed().virtualization_mode`.
    core::VirtualizationMode virtualization_mode = core::VirtualizationMode::Kvm;
    /// Rust `snapshot.committed().runtime_versions.{tools_drive,envd}_version`.
    std::string tools_drive_version;
    std::string envd_version;
    /// Rust `snapshot.committed().context.{env_vars,workdir,user}`.
    std::vector<std::pair<std::string, std::string> > env_vars;
    std::string workdir;
    core::Optional<std::string> user;
};

/// The parts of a `SandboxSnapshotManifest` the projection reads. Kept separate
/// from `RunnableSnapshotView` because these come from the hydrated node-local
/// manifest, not from the published record.
struct SnapshotManifestView {
    /// Rust `manifest.backend` — the VMM that took the capture.
    std::string backend;
    std::string vm_state_path;
    std::string memory_image_config_path;
    uint64_t memory_virtual_size = 0;
    std::string rootfs_image_config_path;
    uint64_t rootfs_virtual_size = 0;
    std::vector<ExtraDrive> extra_drives;
    std::size_t volume_drive_slots = 0;
    std::size_t physical_extra_drive_count = 0;
};

/// Rust: firecracker/config.rs :: FirecrackerSnapshotConfig — a resume.
struct SnapshotConfig {
    CommonConfig common;
    /// Path to the Firecracker VM state file (for example `vm_state.bin`).
    std::string vm_state_path;
    /// The memory overlaybd image config.
    ublk::OverlaybdDeviceConfig mem_overlaybd_config;
    /// Virtual size of the memory image, in bytes.
    uint64_t mem_virtual_size = 0;

    /// Rust `#[serde(skip)] pack_recording` — marks a throwaway VM booted only
    /// to record the startup memory pack at publish time. Runtime-only.
    bool pack_recording = false;

    /// Rust `FirecrackerSnapshotConfig::validate`.
    core::Expected<core::Unit, std::string> Validate() const;

    /// Rust `FirecrackerSnapshotConfig::validate_persisted` — validates stored
    /// artifacts while deferring the node's launch dependencies to resume time.
    core::Expected<core::Unit, std::string> ValidatePersisted() const;

    /// Rust `FirecrackerSnapshotConfig::validate_persisted_artifacts`.
    core::Expected<core::Unit, std::string> ValidatePersistedArtifacts() const;
};

/// Rust: firecracker/config.rs :: FirecrackerSnapshotConfig::from_runnable_snapshot
/// — project a committed snapshot into the config that resumes it.
///
/// Three things are refused up front, because each would otherwise fail deep
/// inside a resume with a far less obvious symptom:
///   * a capture taken by a different VMM — a snapshot is restored by the VMM
///     that took it, so another backend's capture is unusable here;
///   * a capture taken under a different virtualization mode — the guest ABI
///     differs, and the error names both modes so an operator knows which node
///     to move it to;
///   * a record with no tools drive version — see
///     [`CommonConfig::ResolvedToolsDrivePath`].
///
/// `base` supplies the node's own defaults (Rust calls `from_global_config()`
/// internally); the snapshot's recorded values are layered on top.
core::Expected<SnapshotConfig, std::string> SnapshotConfigFromRunnable(
    const CommonConfig& base, const cfg::AppConfig& app_config,
    const RunnableSnapshotView& snapshot, const SnapshotManifestView& manifest);

/// Rust: firecracker/factory.rs :: is_allowed_extra_boot_arg — one token.
///
/// A token is allowed when every character is in
/// `[A-Za-z0-9_\-.\/+=]` AND it starts with one of the non-empty
/// `allowed_prefixes`. The character allowlist is a security boundary: it
/// prevents a carefully crafted API value from injecting shell metacharacters
/// or Firecracker API escapes into the kernel command line. Both conditions
/// must hold — a well-formed token with no matching prefix is dropped.
bool IsAllowedExtraBootArg(const std::string& arg,
                           const std::vector<std::string>& allowed_prefixes);

/// Rust: firecracker/factory.rs :: filter_extra_boot_args — the whole string.
///
/// Trims whitespace, splits on runs of whitespace, applies
/// [`IsAllowedExtraBootArg`] to each token, and rejoins survivors. Returns
/// `nullopt` when the input is absent, blank, or every token is rejected —
/// matching Rust's `then(|| filtered.join(" "))`.
core::Optional<std::string> FilterExtraBootArgs(
    const core::Optional<std::string>& extra_boot_args,
    const std::vector<std::string>& allowed_prefixes);

/// Rust: firecracker/config.rs :: validate_overlaybd_extra_drive_set —
/// enforces the MAX_EXTRA_DRIVES ceiling, unique drive ids, non-overlapping
/// normalised mount paths, non-zero virtual sizes and image config existence.
core::Expected<core::Unit, std::string>
    ValidateExtraDriveSet(const std::vector<ExtraDrive>& drives,
                          bool check_image_exists);

/// Rust: firecracker/config.rs :: resolve_serial_output_dir — turns a relative
/// dir into an absolute one (joined onto cwd); empty input yields empty output.
core::Expected<std::string, std::string>
    ResolveSerialOutputDir(const std::string& dir);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_CONFIG_H_
