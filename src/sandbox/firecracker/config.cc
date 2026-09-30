// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/config.rs
//
// The config *data* lives in the header; this TU carries the logic that decides
// whether a launch or a resume is attempted: the constructors that project an
// AppConfig into a launch config, and the layered validators.
#include "agentenv/sandbox/firecracker/config.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "agentenv/sandbox/manifest.h"
#include "agentenv/sandbox/firecracker/lifecycle.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

// Rust: DEFAULT_BOOT_ARGS. Rust builds this from a `\`-continued string
// literal, which strips both the newline and the next line's indentation, so
// the joined form is single-spaced. Kept token-for-token in sync with
// `config/default.toml`; the DAMON values are reclaim *policy*, so a stale copy
// here silently changes how aggressively guest pagecache is dropped.
const char* const kDefaultBootArgs =
    "console=ttyS0 reboot=k panic=1 pci=off "
    "damon_reclaim.enabled=Y "
    "damon_reclaim.min_age=100000 "
    "damon_reclaim.quota_ms=20 "
    "damon_reclaim.quota_sz=1073741824 "
    "damon_reclaim.quota_reset_interval_ms=500 "
    "damon_reclaim.wmarks_high=990 "
    "damon_reclaim.wmarks_mid=990 "
    "damon_reclaim.wmarks_low=200 "
    "damon_reclaim.skip_anon=Y "
    "damon_reclaim.wmarks_interval=1000000";

namespace {

bool PathExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool PathIsFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// Rust's `.map(|l| l.trim().to_string()).filter(|l| !l.is_empty())`: a level
/// that trims to nothing is the same as no level at all.
core::Optional<std::string> NormalizeLogLevel(const core::Optional<std::string>& level) {
    if (!level.has_value()) return core::Optional<std::string>();
    const std::string& raw = *level;
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    if (begin == end) return core::Optional<std::string>();
    return core::Optional<std::string>(raw.substr(begin, end - begin));
}

bool TrimsToEmpty(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

}  // namespace

// ── RuntimePolicy ───────────────────────────────────────────────────────────

RuntimePolicy RuntimePolicy::FromAppConfig(const cfg::AppConfig& config) {
    RuntimePolicy policy;
    // Rust stores Durations built with `from_secs` / `from_millis`; the unit
    // conversion happens here so the struct holds one unit throughout.
    policy.socket_timeout_ms = config.firecracker.socket_timeout_secs * 1000;
    policy.socket_poll_interval_ms = config.firecracker.socket_poll_ms;
    policy.envd_timeout_ms = config.envd.init_timeout_secs * 1000;
    policy.envd_poll_interval_ms = config.envd.poll_ms;
    return policy;
}

// ── CommonConfig ────────────────────────────────────────────────────────────

CommonConfig CommonConfig::New(const std::string& firecracker_binary,
                               const std::string& tools_drive_version,
                               const RuntimePolicy& runtime_policy) {
    CommonConfig common;
    common.firecracker_binary = firecracker_binary;
    common.tools_drive_version = tools_drive_version;
    common.runtime_policy = runtime_policy;
    // Rust's `new` seeds these three from defaults rather than leaving them
    // zero, so a config built outside `from_app_config` is still launchable.
    common.track_dirty_pages = true;
    common.envd_version = cfg::EnvdConfig().version;
    common.control_plane_port = cfg::ToolsConfig().control_plane_port;
    return common;
}

core::Expected<CommonConfig, std::string> CommonConfig::FromAppConfig(
    const cfg::AppConfig& config) {
    CommonConfig common = New(config.ResolvedFirecrackerBinaryPath(),
                              config.ResolvedToolsVersion(),
                              RuntimePolicy::FromAppConfig(config));
    common.envd_version = config.envd.version;
    common.disk_rate_limit = config.machine.disk_rate_limit;
    common.track_dirty_pages = config.memory_snapshot.track_dirty_pages;
    common.rootfs_allow_shrink = config.ublk.overlaybd.allow_shrink;
    common.control_plane_port = config.tools.control_plane_port;
    common.firecracker_work_base_dir = config.firecracker.work_dir;

    if (config.firecracker.serial_dir.has_value()) {
        core::Expected<std::string, std::string> resolved =
            ResolveSerialOutputDir(*config.firecracker.serial_dir);
        if (!resolved.ok()) return core::make_unexpected(resolved.error());
        common.serial_output_base_dir = resolved.value();
    }
    common.firecracker_log_level = NormalizeLogLevel(config.firecracker.log_level);
    return core::Expected<CommonConfig, std::string>(common);
}

core::Expected<std::string, std::string> CommonConfig::ResolvedToolsDrivePath(
    const cfg::AppConfig& config) const {
    // An empty version is not "use the current drive": the sandbox was captured
    // against a specific tools release, and resuming it against a different one
    // would change the guest's userland underneath a restored memory image.
    if (TrimsToEmpty(tools_drive_version)) {
        return core::make_unexpected(std::string(
            "sandbox state does not record a tools drive version; migrate its "
            "persisted metadata before resuming it"));
    }
    core::Expected<std::string, std::string> resolved =
        config.ResolvedToolsDrivePathForVersion(tools_drive_version);
    if (!resolved.ok()) {
        return core::make_unexpected(std::string("resolve tools drive version '") +
                                     tools_drive_version + "' under dependency root " +
                                     config.deps_path + ": " + resolved.error());
    }
    return resolved;
}

core::Expected<core::Unit, std::string> CommonConfig::ValidatePersistedArtifacts() const {
    core::Expected<core::Unit, std::string> drives =
        ValidateExtraDriveSet(extra_drives, true);
    if (!drives.ok()) return core::make_unexpected(drives.error());

    // Rust matches `Some(0)`: an *absent* size means "let the daemon read it
    // from the image", which is valid; a recorded zero is a corrupt record.
    if (rootfs_virtual_size.has_value() && *rootfs_virtual_size == 0) {
        return core::make_unexpected(std::string("rootfs virtual size must be non-zero"));
    }
    if (ublk_config.has_value() &&
        !PathExists(ublk_config->overlaybd.image_config_path)) {
        return core::make_unexpected(std::string("overlaybd image config not found at ") +
                                     ublk_config->overlaybd.image_config_path);
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string> CommonConfig::Validate() const {
    // Order follows Rust: the tools drive version is checked first because a
    // config that cannot name its tools drive is unlaunchable regardless of
    // what else is on this host.
    //
    // Rust reaches the global config directly (it panics if uninitialized); the
    // C++ accessor returns null instead, so an uninitialized global is reported
    // as the configuration error it is rather than crashing a running node.
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();
    if (config == nullptr) {
        return core::make_unexpected(
            std::string("global configuration is not initialized"));
    }
    core::Expected<std::string, std::string> tools = ResolvedToolsDrivePath(*config);
    if (!tools.ok()) return core::make_unexpected(tools.error());
#ifndef __linux__
    return core::make_unexpected(std::string("Firecracker requires a Linux host"));
#endif
    if (!PathExists(firecracker_binary)) {
        return core::make_unexpected(std::string("firecracker binary not found at ") +
                                     firecracker_binary);
    }
    return ValidatePersistedArtifacts();
}

// Rust `logging_enabled` is defined in lifecycle.cc; declared in lifecycle.h.

// ── SandboxConfig (fresh boot) ──────────────────────────────────────────────

SandboxConfig SandboxConfig::New(const std::string& firecracker_binary,
                                 const std::string& kernel_image,
                                 const std::string& tools_drive_version,
                                 const std::string& user_image_config_path) {
    const cfg::AppConfig app_config;
    SandboxConfig config;
    config.common = CommonConfig::New(firecracker_binary, tools_drive_version,
                                      RuntimePolicy::FromAppConfig(app_config));
    ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = user_image_config_path;
    // A fresh boot's user rootfs is writable: this is the sandbox's own disk,
    // not a shared base image.
    rootfs.read_only = false;
    rootfs.runtime_upper_mode = storage::overlaybd::UpperMode::LogStructured;
    config.common.rootfs_image_config = rootfs;
    config.common.disk_rate_limit = app_config.machine.disk_rate_limit;
    config.kernel_image = kernel_image;
    config.vcpu_count = app_config.machine.vcpu_count;
    config.mem_size_mib = app_config.machine.mem_size_mib;
    return config;
}

core::Expected<core::Unit, std::string> SandboxConfig::Validate() const {
    core::Expected<core::Unit, std::string> common_ok = common.Validate();
    if (!common_ok.ok()) return core::make_unexpected(common_ok.error());
    if (!PathExists(kernel_image)) {
        return core::make_unexpected(std::string("kernel image not found at ") + kernel_image);
    }
    if (!common.rootfs_image_config.has_value()) {
        return core::make_unexpected(std::string("user image overlaybd config is missing"));
    }
    const std::string& path = common.rootfs_image_config->image_config_path;
    if (!PathExists(path)) {
        return core::make_unexpected(
            std::string("user image overlaybd config not found at ") + path);
    }
    // Existence is not enough: a directory at this path would be opened as a
    // config and fail much later, inside the daemon.
    if (!PathIsFile(path)) {
        return core::make_unexpected(
            std::string("user image overlaybd config path is not a file: ") + path);
    }
    return core::Unit{};
}

// ── SnapshotConfig (resume) ─────────────────────────────────────────────────

core::Expected<core::Unit, std::string> SnapshotConfig::ValidatePersistedArtifacts() const {
    // Unlike a fresh boot, a resume *requires* a recorded rootfs size: the
    // restored guest expects the block device it was captured with.
    if (!common.rootfs_virtual_size.has_value() || *common.rootfs_virtual_size == 0) {
        return core::make_unexpected(std::string("rootfs virtual size must be non-zero"));
    }
    if (!common.rootfs_image_config.has_value()) {
        return core::make_unexpected(std::string("base rootfs image config is missing"));
    }
    if (!PathExists(vm_state_path)) {
        return core::make_unexpected(std::string("vm state snapshot not found at ") +
                                     vm_state_path);
    }
    const std::string& mem_path = mem_overlaybd_config.image_config_path;
    if (!PathExists(mem_path)) {
        return core::make_unexpected(
            std::string("mem overlaybd image config not found at ") + mem_path);
    }
    const std::string& rootfs_path = common.rootfs_image_config->image_config_path;
    if (!PathExists(rootfs_path)) {
        return core::make_unexpected(std::string("base rootfs not found at ") + rootfs_path);
    }
    if (!PathIsFile(mem_path)) {
        return core::make_unexpected(
            std::string("mem overlaybd image config path is not a file: ") + mem_path);
    }
    if (!PathIsFile(rootfs_path)) {
        return core::make_unexpected(std::string("base rootfs path is not a file: ") +
                                     rootfs_path);
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string> SnapshotConfig::Validate() const {
    core::Expected<core::Unit, std::string> common_ok = common.Validate();
    if (!common_ok.ok()) return core::make_unexpected(common_ok.error());
    return ValidatePersistedArtifacts();
}

core::Expected<SnapshotConfig, std::string> SnapshotConfigFromRunnable(
    const CommonConfig& base, const cfg::AppConfig& app_config,
    const RunnableSnapshotView& snapshot, const SnapshotManifestView& manifest) {
    // A capture is restored by the VMM that took it, so another backend's
    // snapshot is not merely unsupported here — it is unreadable.
    if (manifest.backend != kFirecrackerBackend) {
        return core::make_unexpected(
            std::string("snapshot '") + snapshot.snapshot_id + "' was captured by the " +
            manifest.backend +
            " backend, and a capture is restored by the VMM which took it");
    }
    // The guest ABI differs between modes, so a capture can only be resumed
    // under the one it was taken in. Both modes are named so an operator can
    // see which kind of node this snapshot belongs on.
    if (snapshot.virtualization_mode != app_config.virtualization_mode) {
        return core::make_unexpected(
            std::string("snapshot '") + snapshot.snapshot_id + "' uses virtualization mode '" +
            core::VirtualizationModeToString(snapshot.virtualization_mode) +
            "', but this node runs in mode '" +
            core::VirtualizationModeToString(app_config.virtualization_mode) + "'");
    }
    if (TrimsToEmpty(snapshot.tools_drive_version)) {
        return core::make_unexpected(std::string(
            "snapshot does not record a tools drive version; migrate its metadata "
            "before launching it"));
    }

    SnapshotConfig config;
    config.common = base;
    config.common.tools_drive_version = snapshot.tools_drive_version;
    config.common.envd_version = snapshot.envd_version;

    // The rootfs takes this *node's* overlaybd settings, not the ones the
    // capture was built with: read_only and the upper format are properties of
    // how this host runs the image, not of the image itself.
    ublk::OverlaybdDeviceConfig rootfs;
    rootfs.image_config_path = manifest.rootfs_image_config_path;
    rootfs.read_only = app_config.ublk.overlaybd.read_only;
    rootfs.runtime_upper_mode = app_config.ublk.overlaybd.runtime_upper_mode;
    config.common.rootfs_image_config = rootfs;
    config.common.rootfs_virtual_size = manifest.rootfs_virtual_size;
    config.common.ublk_config = ublk::UblkConfig::OverlaybdWithRuntimeUpperMode(
        rootfs.image_config_path, rootfs.read_only, rootfs.runtime_upper_mode);

    // Rust `(!env_vars.is_empty()).then_some(env_vars)`: absent and empty are
    // distinct downstream, so an empty set must not become an empty map.
    if (!snapshot.env_vars.empty()) {
        config.common.env_vars = snapshot.env_vars;
    }
    config.common.default_workdir = snapshot.workdir;
    config.common.default_user = snapshot.user;

    // A resume already carries the full CPU state inside vm_state.bin, so the
    // pre-boot `PUT /cpu-config` must not happen; re-applying a template would
    // be wrong and Firecracker rejects it anyway.
    //
    // Rust relies on `..base_common` here, which is safe *there* because its
    // base always comes from `from_global_config()` and only the fresh-boot
    // factory ever fills this field in. This signature takes `base` from the
    // caller, so the same guarantee does not hold — cleared explicitly rather
    // than left to a precondition no type enforces.
    config.common.cpu_config_json = core::nullopt;

    config.common.extra_drives = manifest.extra_drives;
    config.common.volume_drive_slots = manifest.volume_drive_slots;
    // Snapshots taken before reserved volume slots existed have no way to
    // distinguish physical drives from slots, and every drive they hold is
    // physical. Trusting the recorded count for those would read 0 and make
    // the launch rebind real drives as if they were free slots.
    config.common.physical_extra_drive_count =
        manifest.volume_drive_slots == 0 ? manifest.extra_drives.size()
                                         : manifest.physical_extra_drive_count;

    config.vm_state_path = manifest.vm_state_path;
    // The memory image is always read-only and log-structured regardless of the
    // node's rootfs settings: it is a captured image being replayed, never
    // written back to.
    config.mem_overlaybd_config.image_config_path = manifest.memory_image_config_path;
    config.mem_overlaybd_config.read_only = true;
    config.mem_overlaybd_config.runtime_upper_mode =
        storage::overlaybd::UpperMode::LogStructured;
    config.mem_virtual_size = manifest.memory_virtual_size;
    config.pack_recording = false;

    return core::Expected<SnapshotConfig, std::string>(config);
}

core::Expected<core::Unit, std::string> SnapshotConfig::ValidatePersisted() const {
    // The distinction from `Validate` is the whole point of this method: a
    // paused state is decoded and checked on nodes that are not about to
    // resume it, so the host's own launch dependencies (Linux, the firecracker
    // binary) must not be required here.
    core::Expected<core::Unit, std::string> common_ok = common.ValidatePersistedArtifacts();
    if (!common_ok.ok()) return core::make_unexpected(common_ok.error());
    return ValidatePersistedArtifacts();
}

// ── extra boot arg filtering ─────────────────────────────────────────────────

bool IsAllowedExtraBootArg(const std::string& arg,
                           const std::vector<std::string>& allowed_prefixes) {
    if (arg.empty()) return false;
    // Rust's character allowlist: alphanumeric or one of `_-.\/+=`.
    // This is a security boundary: it prevents a crafted API value from
    // injecting shell metacharacters or Firecracker API escapes into the
    // kernel command line.
    for (std::size_t i = 0; i < arg.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(arg[i]);
        const bool valid_char =
            std::isalnum(c) || c == '_' || c == '-' || c == '.' ||
            c == '/' || c == '+' || c == '=';
        if (!valid_char) return false;
    }
    // A well-formed token with no matching prefix is still dropped: the prefix
    // list is an explicit allow list, not a charset-only sanitizer.
    for (std::size_t i = 0; i < allowed_prefixes.size(); ++i) {
        if (allowed_prefixes[i].empty()) continue;
        if (arg.compare(0, allowed_prefixes[i].size(), allowed_prefixes[i]) == 0)
            return true;
    }
    return false;
}

core::Optional<std::string> FilterExtraBootArgs(
    const core::Optional<std::string>& extra_boot_args,
    const std::vector<std::string>& allowed_prefixes) {
    if (!extra_boot_args.has_value()) return core::Optional<std::string>();

    // Split on whitespace the same way Rust's `split_whitespace` does.
    std::vector<std::string> tokens;
    {
        const std::string& s = *extra_boot_args;
        std::size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            const std::size_t start = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i > start) tokens.push_back(s.substr(start, i - start));
        }
    }
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (IsAllowedExtraBootArg(tokens[i], allowed_prefixes))
            kept.push_back(tokens[i]);
    }
    if (kept.empty()) return core::Optional<std::string>();

    std::string joined;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i != 0) joined += " ";
        joined += kept[i];
    }
    return core::Optional<std::string>(joined);
}

// ── extra drive set ─────────────────────────────────────────────────────────

core::Expected<core::Unit, std::string>
ValidateExtraDriveSet(const std::vector<ExtraDrive>& drives, bool check_image_exists) {
    if (drives.size() > kMaxExtraDrives) {
        return core::make_unexpected(
            std::string("too many extra drives: at most 24 are supported "
                        "(/dev/vdc..=/dev/vdz)"));
    }

    std::set<std::string> drive_ids;
    // Normalized before comparison, so `//a///b/.` and `/a/b` cannot both be
    // accepted for one mount point.
    std::vector<std::string> mount_paths;

    for (std::size_t i = 0; i < drives.size(); ++i) {
        const ExtraDrive& d = drives[i];

        // The shared validator, not a local copy: a second spelling of the rule
        // could drift from the one `ExtraDrive` itself enforces.
        const core::Expected<core::Unit, std::string> id_valid = ValidateDriveId(d.drive_id);
        if (!id_valid.ok()) {
            return core::make_unexpected(std::string("invalid extra drive id: ") + d.drive_id);
        }
        if (!drive_ids.insert(d.drive_id).second) {
            return core::make_unexpected(std::string("duplicate extra drive id: ") + d.drive_id);
        }

        const core::Expected<std::string, std::string> normalized =
            NormalizeMountPath(d.mount_path);
        if (!normalized.ok()) {
            return core::make_unexpected(std::string("invalid extra drive mount path: ") +
                                         d.mount_path);
        }
        // Overlap, not just equality: `/mnt/a` and `/mnt/a/b` are distinct
        // strings but would shadow each other inside the guest, leaving the
        // nested drive unreachable.
        for (std::size_t existing = 0; existing < mount_paths.size(); ++existing) {
            if (MountPathsOverlap(mount_paths[existing], normalized.value())) {
                return core::make_unexpected(
                    std::string("overlapping extra drive mount path: ") + d.mount_path);
            }
        }
        mount_paths.push_back(normalized.value());

        if (d.virtual_size.has_value() && *d.virtual_size == 0) {
            return core::make_unexpected(
                std::string("extra drive virtual size must be non-zero: ") + d.drive_id);
        }
        if (check_image_exists && !PathExists(d.image_config_path)) {
            return core::make_unexpected(std::string("overlaybd image config not found at ") +
                                         d.image_config_path);
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

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
