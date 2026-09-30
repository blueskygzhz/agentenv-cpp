// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — the FirecrackerBackend (a Backend
// impl that drives a Firecracker microVM). Kept under the firecracker/ subdir to
// mirror the Rust module layout 1:1.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/extra_drive.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

// ── snapshot / drive path constants ─────────────────────────────────────────

/// Rust `VM_STATE_FILE_NAME`.
extern const char* const kVmStateFileName;
/// Rust `ROOTFS_DRIVE_PATH` — the tools drive, guest `vda`.
extern const char* const kRootfsDrivePath;
/// Rust `USER_ROOTFS_DRIVE_PATH` — the user image, guest `vdb`.
extern const char* const kUserRootfsDrivePath;

// ── disk I/O rate limiting ──────────────────────────────────────────────────

/// Rust `RATE_LIMIT_REFILL_TIME_MS`.
///
/// Firecracker's `TokenBucket::size` is the number of tokens replenished every
/// `refill_time`, not a per-second rate. Pinning the refill period to 1000 ms
/// makes the configured `*_per_sec` values equal the sustained per-second rate.
static const int64_t kRateLimitRefillTimeMs = 1000;

/// Firecracker API model `TokenBucket`.
struct TokenBucket {
    int64_t refill_time = 0;
    int64_t size = 0;
    /// Absent is meaningful: Firecracker only grants a one-time burst when the
    /// field is present, so a 0 burst must not be sent as `one_time_burst=0`.
    core::Optional<int64_t> one_time_burst;

    TokenBucket() {}
    TokenBucket(int64_t refill, int64_t sz) : refill_time(refill), size(sz) {}

    bool operator==(const TokenBucket& o) const {
        if (refill_time != o.refill_time || size != o.size) return false;
        if (one_time_burst.has_value() != o.one_time_burst.has_value()) return false;
        return !one_time_burst.has_value() || *one_time_burst == *o.one_time_burst;
    }
    bool operator!=(const TokenBucket& o) const { return !(*this == o); }
};

/// Firecracker API model `RateLimiter`.
struct RateLimiter {
    core::Optional<TokenBucket> bandwidth;
    core::Optional<TokenBucket> ops;
};

/// Rust `bandwidth_bucket` — `nullopt` when the dimension is unconfigured.
core::Expected<core::Optional<TokenBucket>, std::string>
    BandwidthBucket(const cfg::DiskRateLimitConfig& cfg);

/// Rust `ops_bucket` — `nullopt` when the dimension is unconfigured.
core::Expected<core::Optional<TokenBucket>, std::string>
    OpsBucket(const cfg::DiskRateLimitConfig& cfg);

/// Rust `disabled_bucket` — a token bucket Firecracker reads as "disable this
/// dimension".
///
/// `PATCH /drives` maps an *absent* token bucket to "leave unchanged", so a
/// snapshot-inherited limit cannot be removed by omission. The explicit disable
/// sentinel is a bucket with both `size == 0` and `refill_time == 0`; a mixed
/// bucket (e.g. `size == 0`, `refill_time == 1`) is not the sentinel and can be
/// rejected as an invalid token bucket, failing the resume PATCH.
TokenBucket DisabledBucket();

/// Rust `build_disk_rate_limiter` — the limiter attached to the user rootfs
/// drive at fresh boot (pre-boot `PUT /drives`). `nullopt` when limiting is
/// disabled or no dimension is configured, in which case the drive is added
/// with no limiter at all.
core::Expected<core::Optional<RateLimiter>, std::string>
    BuildDiskRateLimiter(const cfg::DiskRateLimitConfig& cfg);

/// Rust `reconcile_disk_rate_limiter` — the limiter to PATCH on resume,
/// reconciling a snapshot-inherited limiter against this node's config. BOTH
/// buckets are always present: an unset dimension is overwritten with
/// [`DisabledBucket`] so any inherited limit on it is cleared.
core::Expected<RateLimiter, std::string>
    ReconcileDiskRateLimiter(const cfg::DiskRateLimitConfig& cfg);

// ── drive slots / boot args ─────────────────────────────────────────────────

/// Rust `VOLUME_DRIVE_PLACEHOLDER_SIZE`.
static const uint64_t kVolumeDrivePlaceholderSize = 4096;

/// Rust `volume_drive_slot_id`.
std::string VolumeDriveSlotId(std::size_t index);

/// Rust `rootfs_and_extra_drive_image_config_paths`.
///
/// Takes the two pieces explicitly rather than a whole common config: the
/// rootfs entry is optional in Rust and the caller is the only one that knows
/// whether it is populated.
std::vector<std::string> RootfsAndExtraDriveImageConfigPaths(
    const core::Optional<std::string>& rootfs_image_config_path,
    const std::vector<ExtraDrive>& extra_drives);

/// Rust `build_drives_boot_arg` — builds
/// `agentenv_drives=vdc:/mnt/data,vdd:/mnt/logs:sub/path`. `nullopt` if there
/// are no extra drives.
///
/// Drive letters: `vda` = tools drive, `vdb` = user image, `vdc` = first extra
/// drive. Each entry is `vd<letter>:<mountPath>[:<subPath>]`. API validation
/// rejects `:` in both `mountPath` and `subPath`, so the separators are
/// unambiguous. Mirrors Rust's `assert!` on the drive-count ceiling: the caller
/// is expected to have run `ValidateExtraDriveSet` first.
core::Optional<std::string> BuildDrivesBootArg(
    const std::vector<ExtraDrive>& extra_drives);

// ── DAMON reclaim monitor region ────────────────────────────────────────────

/// Rust `damon_monitor_region` — the guest-physical window DAMON should watch,
/// derived from Firecracker's per-architecture memory layout. `nullopt` for a
/// zero-sized VM or an architecture whose layout is not known here.
core::Optional<std::pair<uint64_t, uint64_t> >
    DamonMonitorRegion(uint32_t mem_size_mib, const std::string& arch);

/// Rust `add_damon_monitor_region` — append the computed region to the kernel
/// boot args.
///
/// An explicitly supplied region wins, so an operator can pin it. The one
/// exception is the `start=0 end=0` pair, which is a request to substitute the
/// computed region: those two tokens are dropped and replaced.
core::Optional<std::string> AddDamonMonitorRegion(
    const core::Optional<std::string>& boot_args, uint32_t mem_size_mib,
    const std::string& arch);

/// Configuration to launch a Firecracker process.
struct Config {
    std::string firecracker_bin = "/usr/bin/firecracker";
    std::string jailer_bin      = "/usr/bin/jailer";
    std::string chroot_base_dir = "/var/lib/agentenv/vm";
    bool        use_jailer      = true;
};

class FirecrackerBackend final : public Backend {
 public:
    explicit FirecrackerBackend(Config cfg);
    ~FirecrackerBackend() override;

    core::Expected<Handle, core::AnyError>
        Boot(LaunchPlan plan) override;
    core::Expected<core::Unit, core::AnyError>
        Shutdown(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Pause(core::SandboxId id) override;
    core::Expected<core::Unit, core::AnyError>
        Resume(core::SandboxId id) override;
    core::Expected<std::string, core::AnyError>
        Snapshot(core::SandboxId id, const std::string& out_dir) override;
    core::Expected<Handle, core::AnyError>
        Restore(LaunchPlan plan, const std::string& snapshot_dir) override;
    core::Expected<ExecResult, core::AnyError>
        Exec(core::SandboxId id, ExecSpec spec) override;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_H_
