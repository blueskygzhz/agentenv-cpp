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

class FirecrackerInstance;
class Connector;

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

/// Rust `IoEngine` — the virtio-blk I/O engine for a drive.
enum class IoEngine {
    Sync,
    Async,
};
const char* IoEngineToString(IoEngine engine);

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

// ── microVM configuration sequence ──────────────────────────────────────────
//
// Rust `FirecrackerSandbox::configure_microvm` — the pre-boot call sequence
// that turns a bare Firecracker process into the machine a sandbox expects.
// Until now the port had every individual API call but nothing that ordered
// them, which is the part that decides whether a VM boots at all.
//
// The order is not cosmetic. Firecracker applies most of these only pre-boot
// and rejects them afterwards, and the drives must be attached in slot order
// because the guest names them positionally (`vda`, `vdb`, `vdc`...). A drive
// added out of order still succeeds at the API, then the guest mounts the
// wrong filesystem.
//
// Modelled as a plan rather than executed inline so the ordering can be tested
// without a VMM. The sequence is what breaks, and a wrong sequence fails deep
// inside a boot where the error names the last call rather than the cause.

/// One pre-boot API call, in the order it must be issued.
struct ConfigureStep {
    enum class Kind {
        Logger,           ///< `PUT /logger`
        MachineConfig,    ///< `PUT /machine-config`
        CpuConfig,        ///< `PUT /cpu-config`
        BootSource,       ///< `PUT /boot-source`
        Drive,            ///< `PUT /drives/{id}`
        NetworkInterface, ///< `PUT /network-interfaces/{id}`
        MmdsConfig,       ///< `PUT /mmds/config`
        Mmds,             ///< `PUT /mmds`
        Balloon,          ///< `PUT /balloon`
    };

    Kind kind = Kind::Balloon;
    /// `Drive` / `NetworkInterface` / `MmdsConfig`: the device id.
    std::string id;
    /// `Drive`: host path. `BootSource`: kernel image. `Logger`: log path.
    std::string path;
    /// `Logger`: level. `CpuConfig`: the template. `BootSource`: boot args.
    std::string text;
    /// `Drive` flags, mirroring `add_drive`'s arguments.
    bool is_root = false;
    bool read_only = false;
    bool direct = false;
    IoEngine io_engine = IoEngine::Sync;
    core::Optional<RateLimiter> rate_limiter;
    /// `MachineConfig`.
    uint32_t mem_size_mib = 0;
    uint32_t vcpu_count = 0;
    bool smt = false;
    bool track_dirty_pages = false;
};

/// What `BuildConfigureSequence` needs to know. Deliberately not the whole
/// sandbox config: this keeps the ordering testable without a work dir, a
/// VMM or a tools-drive version to resolve.
struct ConfigurePlan {
    uint32_t mem_size_mib = 0;
    uint32_t vcpu_count = 0;
    bool track_dirty_pages = false;
    /// Empty means no logger is configured, matching `LoggingEnabled`.
    std::string log_path;
    std::string log_level;
    /// Empty means no CPU template, as Rust checks `!cpu_json.is_empty()`.
    std::string cpu_config_json;
    /// Already canonicalised by the caller, as Rust does before this point.
    std::string kernel_image;
    core::Optional<std::string> boot_args;
    cfg::DiskRateLimitConfig disk_rate_limit;
    /// Physical extra drives, then volume slots. Rust splits one prepared
    /// list at `physical_extra_drive_count`, so the split is validated here.
    std::vector<DriveMount> extra_drive_attachments;
    std::size_t physical_extra_drive_count = 0;
    std::size_t volume_drive_slots = 0;
    /// Rust gates the interface, MMDS config and MMDS document on
    /// `network_slot.is_some()`. All three move together: MMDS is served over
    /// the interface, so configuring it without one leaves the guest unable to
    /// read its own metadata.
    bool has_network_slot = false;
    /// Serialized MMDS document. Only read when `has_network_slot`.
    std::string mmds_json;
};

/// Rust `configure_microvm` — the ordered pre-boot call sequence.
///
/// Fails rather than truncating when the prepared drive list is shorter than
/// `physical_extra_drive_count`, or when there are more volume drives than
/// reserved slots. Both are Rust `ensure!`s, and both would otherwise attach a
/// drive to the wrong slot: Firecracker cannot add a virtio-block device after
/// a snapshot restore, so every volume must occupy a slot reserved on the
/// first boot.
core::Expected<std::vector<ConfigureStep>, std::string>
    BuildConfigureSequence(const ConfigurePlan& plan);

/// Rust `FirecrackerSandbox::configure_microvm`'s executor half — issues an
/// already-ordered sequence against a live instance.
///
/// Split from `BuildConfigureSequence` so the ordering stays testable without a
/// VMM while the dispatch itself stays testable without re-deriving the order.
/// A step that fails aborts the rest: Firecracker applies these pre-boot and a
/// later call cannot repair an earlier rejection.
core::Expected<core::Unit, std::string>
    ApplyConfigureSequence(FirecrackerInstance* instance,
                           const std::vector<ConfigureStep>& steps);

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

    /// Rust `RuntimePolicy::socket_timeout` / `socket_poll_interval`, from
    /// `firecracker.socket_timeout_secs` / `socket_poll_ms`.
    ///
    /// Bounds the wait for the API socket to appear. Without it a Firecracker
    /// that dies on startup would be waited on forever; `WaitForReady` also
    /// reaps the child, so an early exit is reported as such rather than as a
    /// timeout.
    int64_t socket_timeout_ms = 10 * 1000;
    int64_t socket_poll_ms    = 10;

    /// Rust `capture_output` — when set, the VMM's stdout/stderr go to files
    /// in the work directory instead of being inherited. Both or neither, per
    /// `WarmStdioPaths`: half the diagnostics is worse than none, because it
    /// looks complete.
    bool capture_output = true;

    /// Written into `PUT /logger` when non-empty. A blank level turns the VMM
    /// logger off (`LoggingEnabled`), which is not the same as this being
    /// unset: the logger may only be configured once per VM.
    std::string log_level;

    /// Passed to `PUT /cpu-config` verbatim when non-empty.
    std::string cpu_config_json;

    /// Rust `FirecrackerCommonConfig::{track_dirty_pages, volume_drive_slots,
    /// physical_extra_drive_count}`.
    bool        track_dirty_pages = false;
    std::size_t volume_drive_slots = 0;
    std::size_t physical_extra_drive_count = 0;

    cfg::DiskRateLimitConfig disk_rate_limit;
};

class FirecrackerBackend final : public Backend {
 public:
    explicit FirecrackerBackend(Config cfg);
    ~FirecrackerBackend() override;

    core::Expected<Handle, core::AnyError>
        Boot(LaunchPlan plan) override;

    /// Injects the API connector used for `Boot`. Null (the default) means the
    /// real unix-socket connector is built per boot.
    ///
    /// Exists so the boot sequence can be driven end-to-end against a fake
    /// VMM: everything from spawn onwards is otherwise unreachable without
    /// KVM, which is exactly the part that was never executed before.
    void SetConnectorForTesting(std::shared_ptr<Connector> connector);
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
