// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — Firecracker backend.
//
// A full driver needs a running Firecracker binary + KVM, so the API calls that
// require a live microVM remain unavailable in this environment. What *is* real
// here is the pre-boot phase: input validation and the jailer chroot path
// layout, faithfully following the first steps of the Rust `boot` path. Boot
// therefore fails with a precise, actionable error (missing binary / kernel /
// rootfs, or "requires KVM") instead of a blanket "not implemented".
#include "agentenv/sandbox/firecracker/sandbox.h"
#include "agentenv/sandbox/firecracker/config.h"

#include <sys/stat.h>

#include <cassert>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace agentenv {
namespace sandbox {
namespace firecracker {

const char* const kVmStateFileName     = "vm_state.bin";
const char* const kRootfsDrivePath     = "rootfs.ext4";
const char* const kUserRootfsDrivePath = "user-rootfs";

// ── disk I/O rate limiting ──────────────────────────────────────────────────

namespace {

/// Rust's `i64::try_from(u64)`: Firecracker's API models the token bucket
/// fields as signed, so an out-of-range config is a hard error rather than a
/// silently wrapped limit.
core::Expected<int64_t, std::string> ToApiInt(uint64_t value, const char* field) {
    if (value > static_cast<uint64_t>(INT64_MAX)) {
        return core::make_unexpected(std::string("disk ") + field +
                                     " exceeds Firecracker's i64 range");
    }
    return core::Expected<int64_t, std::string>(static_cast<int64_t>(value));
}

/// One dimension of the limiter: `rate` is the sustained per-second value and
/// `burst` the optional one-time allowance. Both fields are named in errors so
/// an operator knows which config key to fix.
core::Expected<core::Optional<TokenBucket>, std::string> MakeBucket(
    uint64_t rate, const char* rate_field, uint64_t burst, const char* burst_field) {
    if (rate == 0) {
        return core::Expected<core::Optional<TokenBucket>, std::string>(
            core::Optional<TokenBucket>());
    }
    auto size = ToApiInt(rate, rate_field);
    if (!size.ok()) return core::make_unexpected(size.error());
    TokenBucket bucket(kRateLimitRefillTimeMs, size.value());
    if (burst > 0) {
        auto one_time = ToApiInt(burst, burst_field);
        if (!one_time.ok()) return core::make_unexpected(one_time.error());
        bucket.one_time_burst = one_time.value();
    }
    return core::Expected<core::Optional<TokenBucket>, std::string>(
        core::Optional<TokenBucket>(bucket));
}

}  // namespace

core::Expected<core::Optional<TokenBucket>, std::string>
BandwidthBucket(const cfg::DiskRateLimitConfig& cfg) {
    return MakeBucket(cfg.bandwidth_bytes_per_sec, "bandwidth_bytes_per_sec",
                      cfg.bandwidth_burst_bytes, "bandwidth_burst_bytes");
}

core::Expected<core::Optional<TokenBucket>, std::string>
OpsBucket(const cfg::DiskRateLimitConfig& cfg) {
    return MakeBucket(cfg.iops, "iops", cfg.iops_burst, "iops_burst");
}

TokenBucket DisabledBucket() { return TokenBucket(0, 0); }

core::Expected<core::Optional<RateLimiter>, std::string>
BuildDiskRateLimiter(const cfg::DiskRateLimitConfig& cfg) {
    if (!cfg.enabled) {
        return core::Expected<core::Optional<RateLimiter>, std::string>(
            core::Optional<RateLimiter>());
    }
    auto bandwidth = BandwidthBucket(cfg);
    if (!bandwidth.ok()) return core::make_unexpected(bandwidth.error());
    auto ops = OpsBucket(cfg);
    if (!ops.ok()) return core::make_unexpected(ops.error());
    if (!bandwidth.value().has_value() && !ops.value().has_value()) {
        return core::Expected<core::Optional<RateLimiter>, std::string>(
            core::Optional<RateLimiter>());
    }
    RateLimiter rl;
    rl.bandwidth = bandwidth.value();
    rl.ops = ops.value();
    return core::Expected<core::Optional<RateLimiter>, std::string>(
        core::Optional<RateLimiter>(rl));
}

core::Expected<RateLimiter, std::string>
ReconcileDiskRateLimiter(const cfg::DiskRateLimitConfig& cfg) {
    // Start from "both dimensions explicitly disabled" and only replace what
    // this node actually configures. That ordering is the whole point of this
    // function: whatever is left at the sentinel clears the corresponding
    // limit the snapshot brought with it.
    RateLimiter rl;
    rl.bandwidth = DisabledBucket();
    rl.ops = DisabledBucket();
    if (!cfg.enabled) {
        return core::Expected<RateLimiter, std::string>(rl);
    }
    auto bandwidth = BandwidthBucket(cfg);
    if (!bandwidth.ok()) return core::make_unexpected(bandwidth.error());
    auto ops = OpsBucket(cfg);
    if (!ops.ok()) return core::make_unexpected(ops.error());
    if (bandwidth.value().has_value()) rl.bandwidth = *bandwidth.value();
    if (ops.value().has_value()) rl.ops = *ops.value();
    return core::Expected<RateLimiter, std::string>(rl);
}

// ── drive slots / boot args ─────────────────────────────────────────────────

std::string VolumeDriveSlotId(std::size_t index) {
    return std::string(kVolumeDriveSlotPrefix) + std::to_string(index);
}

std::vector<std::string> RootfsAndExtraDriveImageConfigPaths(
    const core::Optional<std::string>& rootfs_image_config_path,
    const std::vector<ExtraDrive>& extra_drives) {
    std::vector<std::string> paths;
    if (rootfs_image_config_path.has_value()) {
        paths.push_back(*rootfs_image_config_path);
    }
    for (std::size_t i = 0; i < extra_drives.size(); ++i) {
        paths.push_back(extra_drives[i].image_config_path);
    }
    return paths;
}

core::Optional<std::string> BuildDrivesBootArg(
    const std::vector<ExtraDrive>& extra_drives) {
    if (extra_drives.empty()) return core::Optional<std::string>();
    assert(extra_drives.size() <= kMaxExtraDrives &&
           "too many extra drives for guest naming");
    std::string joined;
    for (std::size_t i = 0; i < extra_drives.size(); ++i) {
        if (i != 0) joined += ",";
        const char dev_letter = static_cast<char>('c' + static_cast<char>(i));
        joined += "vd";
        joined += dev_letter;
        joined += ":";
        joined += extra_drives[i].mount_path;
        if (extra_drives[i].sub_path.has_value()) {
            joined += ":";
            joined += *extra_drives[i].sub_path;
        }
    }
    return core::Optional<std::string>("agentenv_drives=" + joined);
}

// ── DAMON reclaim monitor region ────────────────────────────────────────────

namespace {

const uint64_t MIB = 1024ULL * 1024ULL;
const uint64_t GIB = 1024ULL * MIB;

const char* const kMonitorRegionStart = "damon_reclaim.monitor_region_start=";
const char* const kMonitorRegionEnd   = "damon_reclaim.monitor_region_end=";

/// Rust `str::split_whitespace`. Boot args are ASCII, so the ASCII-only
/// classification is equivalent here.
std::vector<std::string> SplitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool StartsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::string(prefix).size(), prefix) == 0;
}

}  // namespace

core::Optional<std::pair<uint64_t, uint64_t> >
DamonMonitorRegion(uint32_t mem_size_mib, const std::string& arch) {
    // Rust guards this multiply with `checked_mul`; a u32 count of MiB cannot
    // overflow u64, so the only rejected input is a zero-sized VM.
    const uint64_t memory_size = static_cast<uint64_t>(mem_size_mib) * MIB;
    if (memory_size == 0) return core::Optional<std::pair<uint64_t, uint64_t> >();

    if (arch == "x86_64") {
        uint64_t end;
        if (memory_size <= 3 * GIB) {
            end = memory_size;
        } else if (memory_size <= 255 * GIB) {
            end = memory_size + GIB;
        } else {
            end = memory_size + 257 * GIB;
        }
        return core::Optional<std::pair<uint64_t, uint64_t> >(
            std::pair<uint64_t, uint64_t>(4096, end));
    }
    if (arch == "aarch64") {
        const uint64_t end = memory_size <= 254 * GIB ? 2 * GIB + memory_size
                                                      : memory_size + 258 * GIB;
        return core::Optional<std::pair<uint64_t, uint64_t> >(
            std::pair<uint64_t, uint64_t>(2 * GIB, end));
    }
    return core::Optional<std::pair<uint64_t, uint64_t> >();
}

core::Optional<std::string> AddDamonMonitorRegion(
    const core::Optional<std::string>& boot_args, uint32_t mem_size_mib,
    const std::string& arch) {
    if (!boot_args.has_value()) return core::Optional<std::string>();
    std::string args = *boot_args;

    const std::vector<std::string> tokens = SplitWhitespace(args);
    bool has_monitor_region = false;
    bool has_zero_start = false;
    bool has_zero_end = false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (StartsWith(tokens[i], kMonitorRegionStart) ||
            StartsWith(tokens[i], kMonitorRegionEnd)) {
            has_monitor_region = true;
        }
        if (tokens[i] == std::string(kMonitorRegionStart) + "0") has_zero_start = true;
        if (tokens[i] == std::string(kMonitorRegionEnd) + "0") has_zero_end = true;
    }
    const bool use_computed_region = has_zero_start && has_zero_end;
    // An operator-pinned region wins; only the `0`/`0` pair asks to be filled in.
    if (has_monitor_region && !use_computed_region) {
        return core::Optional<std::string>(args);
    }

    core::Optional<std::pair<uint64_t, uint64_t> > region =
        DamonMonitorRegion(mem_size_mib, arch);
    if (!region.has_value()) return core::Optional<std::string>(args);

    if (use_computed_region) {
        std::string rebuilt;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == std::string(kMonitorRegionStart) + "0" ||
                tokens[i] == std::string(kMonitorRegionEnd) + "0") {
                continue;
            }
            if (!rebuilt.empty()) rebuilt += " ";
            rebuilt += tokens[i];
        }
        args = rebuilt;
    }
    return core::Optional<std::string>(
        args + " " + kMonitorRegionStart + std::to_string(region->first) + " " +
        kMonitorRegionEnd + std::to_string(region->second));
}

class FirecrackerBackend::Impl {
 public:
    explicit Impl(Config c) : cfg(std::move(c)) {}
    Config cfg;

    // Rust: jailer lays out {chroot_base}/firecracker/{sandbox_id}/root .
    std::string ChrootPath(const core::SandboxId& id) const {
        return cfg.chroot_base_dir + "/firecracker/" + id.ToString() + "/root";
    }
    // The API socket lives inside the chroot when jailed.
    std::string ApiSocketPath(const core::SandboxId& id) const {
        return ChrootPath(id) + "/run/firecracker.socket";
    }
};

FirecrackerBackend::FirecrackerBackend(Config cfg)
    : impl_(new Impl(std::move(cfg))) {}
FirecrackerBackend::~FirecrackerBackend() = default;

namespace {
template <typename T>
core::Expected<T, core::AnyError> ready_err(const std::string& msg) {
    return core::make_unexpected(core::err(msg));
}
template <typename T>
core::Expected<T, core::AnyError> ready_ok(T value) {
    return core::Expected<T, core::AnyError>(std::move(value));
}
bool is_file(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

core::Expected<Handle, core::AnyError>
FirecrackerBackend::Boot(LaunchPlan plan) {
    // --- pre-boot validation (this part is real and testable) ---
    //
    // This checks only what the plan actually carries. The full Rust validator
    // is `SandboxConfig::Validate`, which additionally resolves the tools drive
    // version against the global config; this backend skeleton is not wired to
    // an AppConfig, so calling it would fail for a reason unrelated to the
    // caller's request.
    if (impl_->cfg.firecracker_bin.empty() || !exists(impl_->cfg.firecracker_bin)) {
        return ready_err<Handle>(std::string("firecracker boot: firecracker binary not found at ") +
                                 impl_->cfg.firecracker_bin);
    }
    if (!is_file(plan.kernel_path)) {
        return ready_err<Handle>(std::string("firecracker boot: kernel image not found at ") +
                                 plan.kernel_path);
    }
    if (!plan.rootfs_path.empty() && !is_file(plan.rootfs_path)) {
        return ready_err<Handle>(std::string("firecracker boot: rootfs not found at ") +
                                 plan.rootfs_path);
    }
    // KVM is required to actually launch; we can only get this far without it.
    if (!exists("/dev/kvm")) {
        return ready_err<Handle>(std::string(
            "firecracker boot: /dev/kvm not present; cannot launch a microVM on this host"));
    }
    // The rest (spawn firecracker/jailer, drive the API socket to configure
    // machine/boot-source/drives, then start-instance) needs the binary + KVM.
    return ready_err<Handle>(std::string(
        "firecracker boot: pre-boot checks passed; live VM launch not available in this build "
        "(api socket would be ") + impl_->ApiSocketPath(plan.sandbox_id) + ")");
}

core::Expected<core::Unit, core::AnyError>
FirecrackerBackend::Shutdown(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Shutdown: no live VM in this build");
}
core::Expected<core::Unit, core::AnyError>
FirecrackerBackend::Pause(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Pause: no live VM in this build");
}
core::Expected<core::Unit, core::AnyError>
FirecrackerBackend::Resume(core::SandboxId /*id*/) {
    return ready_err<core::Unit>("firecracker Resume: no live VM in this build");
}
core::Expected<std::string, core::AnyError>
FirecrackerBackend::Snapshot(core::SandboxId /*id*/, const std::string& /*out_dir*/) {
    return ready_err<std::string>("firecracker Snapshot: no live VM in this build");
}
core::Expected<Handle, core::AnyError>
FirecrackerBackend::Restore(LaunchPlan /*plan*/, const std::string& /*snapshot_dir*/) {
    return ready_err<Handle>("firecracker Restore: no live VM in this build");
}
core::Expected<ExecResult, core::AnyError>
FirecrackerBackend::Exec(core::SandboxId /*id*/, ExecSpec /*spec*/) {
    return ready_err<ExecResult>("firecracker Exec: no live VM in this build");
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
