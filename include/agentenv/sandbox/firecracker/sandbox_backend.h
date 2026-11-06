// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — the `impl SandboxBackend for
// FirecrackerSandbox` block (pause / resume / stop and their recovery rule).
//
// `SandboxBackend` is the interface the orchestrator actually drives, and the
// one whose signatures match the Rust trait: `Pause` yields a
// `PausedSandboxState` rather than nothing, and reports a
// `SandboxCaptureError` whose variant tells the caller whether the sandbox is
// still safe to run. Until now it had no implementation at all — the
// Firecracker port implemented only the legacy `Backend` shim, whose
// `Pause(SandboxId) -> Unit` cannot express either of those.
//
// Adding to the shim instead would have meant hard-coding the answer to the
// question the variant exists to answer, so this implements the real trait.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_
#define AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/envd.h"
#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/sandbox/firecracker/dirty_memory.h"
#include "agentenv/sandbox/firecracker/paused_state.h"
#include "agentenv/sandbox/firecracker/sandbox.h"
#include "agentenv/sandbox/ublk.h"
#include "agentenv/sandbox/ublk/device.h"

namespace agentenv {
namespace sandbox {
namespace network {
class Slot;
class SlotNetworkManager;
}  // namespace network
namespace firecracker {
class FirecrackerPool;
}  // namespace firecracker

namespace firecracker {

/// Rust `GUEST_ARP_NOTIFY_BOOT_ARG` — makes the guest announce its address
/// when the link comes up, so the namespace learns the VM's MAC without
/// waiting on the first ARP exchange.
extern const char* const kGuestArpNotifyBootArg;

/// Rust `publish_memory_overlaybd_layer` — writes the captured guest memory
/// out as a sealed LSMT layer.
///
/// Built on `storage::overlaybd::lsmt::CompactTo`, with the live VMM's address
/// space as the source: a `CompactSource` backed by `ProcessVmReader`, so the
/// bytes are pulled straight out of the paused VM with `process_vm_readv`
/// rather than staged through a file.
///
/// Rust writes to `<path>.tmp` and renames on success, removing the temp file
/// on failure. Reproduced exactly: the final name is what the resume path
/// loads, so it must never exist in a half-written state — a reader cannot
/// tell a truncated commit file from a short one.
core::Expected<core::Unit, std::string> PublishMemoryOverlaybdLayer(
    const std::string& output_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t virtual_size, int64_t firecracker_pid);

/// Overrides the memory-layer write. Null means use
/// [`PublishMemoryOverlaybdLayer`].
typedef std::function<core::Expected<core::Unit, std::string>(
    const std::string& data_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t memory_size, int64_t firecracker_pid)>
    MemoryLayerWriter;

/// Rust `FirecrackerSandbox::pause_to_dir` — capture a paused VM's state into
/// a caller-supplied directory.
///
/// The ordering is the substance here, and three rules in it are load-bearing:
///
///   * **Writable volumes are flushed before the VM is paused.** The flush runs
///     `busybox sync` *inside the guest* through envd, and envd cannot be
///     reached once the VM is paused. Reversing these two silently captures
///     volumes with dirty page cache, so the resumed sandbox sees a filesystem
///     that is stale rather than merely old.
///   * **The pid is read before the state snapshot is taken.** Guest memory is
///     pulled out of the live VMM with `process_vm_readv`, so the capture needs
///     a pid that is still valid; reading it later would race a VMM that died
///     during the snapshot and attribute the failure to the wrong step.
///   * **A failed capture removes the directory it was writing into.** A
///     half-written snapshot directory is indistinguishable from a complete one
///     by name, and the resume path would later load a `vm_state.bin` whose
///     memory layer was never finished.
struct PauseToDirSpec {
    /// Destination directory. Created by this call, removed again on failure.
    std::string snapshot_dir;
    /// Rust `has_writable_persistent_volumes` — true when any extra drive is
    /// writable *and* has a snapshot output directory.
    bool has_writable_persistent_volumes = false;
    /// Runs `busybox sync` in the guest. Required when the flag above is set;
    /// Rust fails with "cannot flush writable volumes because envd is not
    /// running" when envd is absent, rather than skipping the flush.
    std::function<core::Expected<core::Unit, std::string>()> flush_volumes;
    /// Overrides the memory-layer write. Null uses the real one.
    MemoryLayerWriter write_memory_layer;

    /// Invoked with the VMM pid the moment it is read, before the state
    /// snapshot is requested. Observation only -- it exists so the ordering
    /// rule above can be asserted, since the pid's *value* is identical
    /// whether it is read before or after and therefore proves nothing.
    std::function<void(int64_t)> on_pid_read;
};

/// Rust `managed_snapshot_base` — `{work_dir}/managed-snapshots`, or
/// `{system-temp}/aenv/managed-snapshots` when `work_dir` is unset.
///
/// This is where a `Snapshot()` with no caller-supplied directory writes.
/// Keeping it a function of the config rather than a constant is the point:
/// captures are large and must land on the filesystem the operator chose for
/// them, not on whatever `/tmp` happens to be.
std::string ManagedSnapshotBase(const core::Optional<std::string>& work_dir);

/// The artifacts a successful capture produced.
struct PauseToDirResult {
    /// `{snapshot_dir}/vm_state.bin`.
    std::string vm_state_path;
    /// `{snapshot_dir}/mem_overlaybd/overlaybd.commit`.
    std::string memory_layer_path;
    /// Total size of the contiguous guest-memory image.
    uint64_t memory_size = 0;
};

/// Rust `enum LaunchMode { Fresh(..), Resume(..) }`.
///
/// Modelled explicitly even though only `Fresh` is implemented. `start_nowait`
/// dispatches on it in Rust, and collapsing it to "there is only fresh" would
/// make the Resume arm an untyped afterthought later — the two paths differ in
/// what they send the VMM, not just in their inputs.
struct LaunchMode {
    enum class Kind {
        Fresh,
        Resume,
    };
    Kind kind = Kind::Fresh;
};

/// What a fresh start needs to materialize its block devices on the host.
///
/// Rust `start_fresh` does this inline before spawning the VMM:
///   * `link_tools_drive`   — `work_dir/rootfs.ext4` -> the tools drive;
///   * `attach_user_rootfs` — a writable overlaybd runtime device from the
///     ublk daemon, linked as `work_dir/user-rootfs`;
///   * `prepare_extra_drives` — one runtime device per extra drive, linked as
///     `work_dir/extra-drive-<id>`;
///   * `prepare_volume_drive_slots` / `bind_volume_drive_slot` — placeholder
///     `agentenv_volume_slot_<N>.img` files, the used ones replaced by a
///     symlink to the volume's attachment.
///
/// Firecracker only ever sees these relative names, so without this step the
/// `PUT /drives` calls point at files that do not exist.
/// Rust `deps::prepare_tools_image` as this layer consumes it: given a tools
/// release version, either an installed overlaybd image config to attach as a
/// shared ublk device, or an empty optional meaning "attach the legacy ext4
/// file at `tools_drive_path` instead".
///
/// Injected rather than called, because the real implementation lives in
/// `setup/deps` and needs the image resolver — neither of which this library
/// links, and deliberately so. Unset (the default) means the ext4 path is
/// used unconditionally, which is what every test wants and what a node with
/// a locally provisioned tools drive gets.
typedef std::function<core::Expected<core::Optional<std::string>, std::string>(
    const std::string& tools_drive_version)>
    ToolsDriveResolver;

struct FreshDrivePlan {
    /// Rust `UblkDeviceManager::global()`. Required: the user rootfs is always
    /// a ublk device, so a missing or unavailable daemon fails the start.
    std::shared_ptr<ublk::DeviceManager> device_manager;
    /// Host path of the tools drive (`resolved_tools_drive_path`). Linked
    /// after `realpath`, as Rust canonicalizes it, so a dangling path fails
    /// here instead of inside the guest.
    ///
    /// The *fallback*, not necessarily what gets attached: when
    /// `tools_drive_resolver` yields an overlaybd image, the shared ublk
    /// device's path is attached instead (Rust `link_tools_drive`).
    std::string tools_drive_path;
    /// Rust `common.tools_drive_version` — which release to prepare. Only
    /// read when `tools_drive_resolver` is set.
    std::string tools_drive_version;
    /// Rust `FirecrackerSandbox::prepare_tools_drive`'s resolution step.
    ToolsDriveResolver tools_drive_resolver;
    /// Rust `common.rootfs_image_config`.
    ublk::OverlaybdDeviceConfig rootfs_image_config;
    /// Rust `common.rootfs_virtual_size` — the requested target size.
    core::Optional<uint64_t> rootfs_virtual_size;
    bool rootfs_allow_shrink = false;
    /// Rust `common.extra_drives` — physical drives first, then volumes.
    std::vector<ExtraDrive> extra_drives;
    /// Rust `ublk.overlaybd.global_config_path`.
    std::string overlaybd_global_config;
    /// Rust `ublk.overlaybd.runtime_upper_mode` — applied to extra drives.
    storage::overlaybd::UpperMode runtime_upper_mode =
        storage::overlaybd::UpperMode::HybridLogStructured;
    /// Rust `ublk.overlaybd.allow_shrink` — `ExtraDrivePrepareMode::Fresh`.
    bool extra_drive_allow_shrink = false;
};

/// Rust `SandboxResources` — the ublk devices and the network slot a start
/// created.
///
/// Released after the VMM has stopped (Rust `SandboxResources::release`): a
/// device torn down under a running guest turns every I/O into an error.
/// Device failures are logged, not returned, as Rust does — the handle is
/// already gone, so nobody could act on them. A network release failure is
/// the one that is returned, after everything else was released.
class FreshDriveResources {
 public:
    FreshDriveResources() {}
    ~FreshDriveResources() { (void)Release(); }

    FreshDriveResources(const FreshDriveResources&) = delete;
    FreshDriveResources& operator=(const FreshDriveResources&) = delete;

    void Adopt(std::shared_ptr<ublk::DeviceManager> manager) { manager_ = std::move(manager); }

    /// Rust `SandboxResources::tools_ublk_device`.
    ///
    /// Held for the sandbox's lifetime rather than dropped after the symlink
    /// is made: the handle *is* the refcount on a device shared with every
    /// other sandbox on the same tools release. Releasing it early would let
    /// the device — and its page cache, the entire reason for sharing — be
    /// torn down under a running guest, turning its tools-drive I/O into
    /// errors. `Release` drops it, which only relinquishes this sandbox's
    /// share; the manager keeps one idle device alive for the next launch.
    void AdoptSharedTools(const ublk::SharedReadOnlyDevice& device) { tools_ = device; }
    void SetRootfs(const ublk::OverlaybdRuntimeDevice& device) {
        rootfs_ = device;
        has_rootfs_ = true;
    }
    void AddExtraDrive(const ublk::OverlaybdRuntimeDevice& device) {
        extra_drives_.push_back(device);
    }

    /// Rust `allocate_network_slot` stores the slot "before any fallible
    /// setup so that `stop()` or `Drop` releases it" — this is that store.
    /// The slot goes back to `manager` on release (pooled or torn down).
    void AdoptNetworkSlot(network::SlotNetworkManager* manager, network::Slot* slot) {
        network_manager_ = manager;
        network_slot_ = slot;
    }
    network::Slot* network_slot() const { return network_slot_; }

    bool empty() const {
        return !has_rootfs_ && extra_drives_.empty() && network_slot_ == NULL;
    }
    const ublk::OverlaybdRuntimeDevice* rootfs() const { return has_rootfs_ ? &rootfs_ : NULL; }
    const std::vector<ublk::OverlaybdRuntimeDevice>& extra_drives() const {
        return extra_drives_;
    }

    /// Rootfs, then extra drives, then the network slot — Rust's order
    /// ("ublk devices go after FC stop and before network cleanup").
    /// Idempotent.
    core::Expected<core::Unit, std::string> Release();

 private:
    std::shared_ptr<ublk::DeviceManager> manager_;
    /// Default-constructed (`valid() == false`) when the legacy ext4 drive is
    /// in use, which is why this is the handle type rather than an optional.
    ublk::SharedReadOnlyDevice tools_;
    ublk::OverlaybdRuntimeDevice rootfs_;
    bool has_rootfs_ = false;
    std::vector<ublk::OverlaybdRuntimeDevice> extra_drives_;
    network::SlotNetworkManager* network_manager_ = NULL;
    network::Slot* network_slot_ = NULL;
};

/// Rust `append_boot_arg`.
void AppendBootArg(core::Optional<std::string>* boot_args, const std::string& arg);

/// Rust `start_fresh`'s boot-arg composition, minus the custom-extension
/// contributions this port does not have: `init=/init` (unless an `init=` is
/// already present), then `agentenv_drives=...`, then `network_args` (the
/// slot's `ip=` and `kGuestArpNotifyBootArg`), then the DAMON monitor region.
core::Optional<std::string> ComposeFreshBootArgs(const core::Optional<std::string>& boot_args,
                                                 const std::vector<ExtraDrive>& extra_drives,
                                                 uint32_t mem_size_mib,
                                                 const std::string& arch,
                                                 const std::vector<std::string>& network_args);

/// The same without network arguments — a start with no network slot.
core::Optional<std::string> ComposeFreshBootArgs(const core::Optional<std::string>& boot_args,
                                                 const std::vector<ExtraDrive>& extra_drives,
                                                 uint32_t mem_size_mib,
                                                 const std::string& arch);

/// Rust `start_fresh`'s network half: the slot is allocated at start, and its
/// `ip=` goes before the DAMON region, so the boot args are composed from
/// these inputs only once the slot exists.
struct FreshNetworkPlan {
    /// Rust `NetworkManager::global()`. Non-owning; process-wide.
    network::SlotNetworkManager* manager = NULL;
    /// Rust `config.common.network_policy`.
    core::Optional<network::SandboxNetworkPolicy> policy;
    /// Inputs to `ComposeFreshBootArgs`, which runs again with the slot's
    /// arguments and replaces `ConfigurePlan::boot_args`.
    core::Optional<std::string> boot_args;
    std::vector<ExtraDrive> extra_drives;
    std::string arch;
    /// Rust `self.mmds_metadata(&config.common)`, serialized. Served once the
    /// interface exists, which is why it lives here and not on the plan.
    std::string mmds_json;
};

/// Rust `prepare_volume_drive_slots` + `bind_volume_drive_slot`.
///
/// Creates `count` placeholder files `agentenv_volume_slot_<i>.img` of
/// `kVolumeDrivePlaceholderSize` bytes in `work_dir` (create-new: an existing
/// file is an error, not reused), then replaces the first
/// `volume_attachments.size()` of them with a symlink to the volume's
/// attachment path. Fails when there are more volumes than slots.
core::Expected<core::Unit, std::string> PrepareVolumeDriveSlots(
    const std::string& work_dir, std::size_t count,
    const std::vector<DriveMount>& volume_attachments);

/// Rust `prepare_volume_drive_slots` alone: creates `count` placeholder files
/// and returns them as slot attachments (`drive_id` = `VolumeDriveSlotId(i)`,
/// `attachment_path` = `<drive_id>.img`, relative to `work_dir`). Fails when
/// `physical + count` exceeds `kMaxExtraDrives`.
core::Expected<std::vector<DriveMount>, std::string> CreateVolumeDriveSlots(
    const std::string& work_dir, std::size_t physical_extra_drive_count, std::size_t count);

/// Rust `bind_volume_drive_slot`: replaces the slot's placeholder with a
/// symlink to `drive.attachment_path`. The slot must exist -- a missing one
/// means it was never reserved, and Firecracker would reopen nothing.
core::Expected<core::Unit, std::string> BindVolumeDriveSlot(const std::string& work_dir,
                                                            const DriveMount& slot,
                                                            const DriveMount& drive);

/// Rust `link_tools_drive` + `attach_user_rootfs` + `prepare_extra_drives`.
///
/// Creates the devices and symlinks in `work_dir`, records each device in
/// `resources` as soon as it exists (so a later failure still releases it),
/// and returns the extra-drive attachments in slot order. Rolls back the
/// symlinks it made on failure, as Rust's `PreparedDrives::cleanup` does.
core::Expected<std::vector<DriveMount>, std::string> PrepareFreshDrives(
    const std::string& work_dir, const FreshDrivePlan& plan, FreshDriveResources* resources);

/// Rust `firecracker_stdout_path` / `firecracker_stderr_path` /
/// `firecracker_stdio_paths` / `firecracker_log_path`.
///
/// Everything lands in the default log directory --
/// `<serial_output_base_dir>/<sandbox_id>` when configured, else
/// `<work_dir>/logs` -- except stdout/stderr, whose directory an explicit
/// `stdout_path`/`stderr_path` overrides. A stdio stream is captured when
/// logging is enabled or its directory was given explicitly.
struct FirecrackerLogPaths {
    std::string default_log_dir;
    core::Optional<std::string> stdout_path;
    core::Optional<std::string> stderr_path;
    /// Target of the VMM logger (used only when logging is enabled).
    std::string logger_path;
    /// The stdout/stderr targets regardless of capture, which is where a
    /// warm entry's logs are relocated to.
    std::string stdout_target;
    std::string stderr_target;
};
FirecrackerLogPaths ResolveFirecrackerLogPaths(
    const core::Optional<std::string>& serial_output_base_dir,
    const core::Optional<std::string>& stdout_dir, const core::Optional<std::string>& stderr_dir,
    bool capture_output, const core::SandboxId& sandbox_id, const std::string& work_dir);

/// Rust `relocate_warm_log` — moves a warm entry's log to the sandbox's own
/// path (copy + remove across devices). Absent or identical source is a no-op.
core::Expected<core::Unit, std::string> RelocateWarmLog(const core::Optional<std::string>& src,
                                                       const std::string& target);

/// What a fresh start needs. Separate from `ConfigurePlan`, which describes the
/// API calls: this is the process-level half (which binary, where it works,
/// how long to wait).
struct FreshStartSpec {
    /// Already-validated plan for the pre-boot API calls.
    ConfigurePlan configure;
    /// Host-side drive materialization. Absent only for callers that attach
    /// pre-made files themselves (the legacy `Backend` shim and tests); the
    /// factory always sets it, because Rust's `start_fresh` always runs it.
    core::Optional<FreshDrivePlan> drives;
    /// Network slot + guest `ip=` + interface/MMDS. Absent means no network
    /// (tests, and a factory given no network manager).
    core::Optional<FreshNetworkPlan> network;
    std::string firecracker_bin;
    /// Parent for the per-instance work directory.
    std::string chroot_base_dir;
    core::SandboxId sandbox_id;
    /// Rust `[firecracker].work_dir`. Only the managed-snapshot base is
    /// derived from it here (see [`ManagedSnapshotBase`]); the per-instance
    /// work directory comes from `chroot_base_dir`. Absent falls back to the
    /// system temporary directory, as Rust does.
    core::Optional<std::string> firecracker_work_dir;
    bool capture_output = true;
    /// Rust `common.{serial_output_base_dir, stdout_path, stderr_path}` --
    /// where the VMM's stdio and logger land (`ResolveFirecrackerLogPaths`).
    core::Optional<std::string> serial_output_base_dir;
    core::Optional<std::string> stdout_path;
    core::Optional<std::string> stderr_path;
    /// Rust `FirecrackerPool::global()`. A fresh boot never adopts a warm
    /// entry; this is only what a fork child of this sandbox resumes with.
    FirecrackerPool* firecracker_pool = NULL;
    int64_t socket_timeout_ms = 10 * 1000;
    int64_t socket_poll_ms = 10;
    /// Rust `RuntimePolicy::{envd_timeout, envd_poll_interval}`.
    int64_t envd_timeout_ms = 60 * 1000;
    int64_t envd_poll_ms = 100;
    /// Rust `init_envd_instance`: `common.control_plane_port` and
    /// `common.envd_access_token`, with the slot's host-interaction IP.
    uint16_t control_plane_port = 49983;
    core::Optional<EnvdAccessToken> envd_access_token;
};

/// Host-side drive materialization for a resume (Rust `start_resume`:
/// `link_tools_drive`, `attach_user_rootfs`, `prepare_resume_drives`).
///
/// The drive list, rootfs config and slot counts come from the snapshot
/// config itself; this carries only what the node supplies.
struct ResumeDrivePlan {
    /// Rust `UblkDeviceManager::global()`.
    std::shared_ptr<ublk::DeviceManager> device_manager;
    /// `common.resolved_tools_drive_path(..)` for the capture's tools version.
    /// The fallback, as on the fresh path.
    std::string tools_drive_path;
    /// The capture's tools release, not the node's: a resumed guest must get
    /// the tools drive it was captured against. Only read when
    /// `tools_drive_resolver` is set.
    std::string tools_drive_version;
    /// Rust `prepare_tools_drive`, which `start_resume` calls too.
    ToolsDriveResolver tools_drive_resolver;
    /// Rust `ublk.overlaybd.global_config_path`.
    std::string overlaybd_global_config;
    /// Rust `ublk.overlaybd.runtime_upper_mode`.
    storage::overlaybd::UpperMode runtime_upper_mode =
        storage::overlaybd::UpperMode::HybridLogStructured;
    /// Rust `ublk.overlaybd.allow_shrink` — only for launch-slot drives new to
    /// this launch, which carry a requested rather than a recorded size.
    bool extra_drive_allow_shrink = false;
};

/// Rust `(slot, drive)` — a reserved slot and the volume attachment that is
/// to replace its placeholder.
struct VolumeSlotBind {
    DriveMount slot;
    DriveMount drive;
};

/// Rust `prepare_resume_drives` (after `link_tools_drive` and
/// `attach_user_rootfs`, which this also performs).
///
/// Snapshot load reopens every serialized slot path, so every placeholder is
/// created here. Volumes that were in the source state are bound now, before
/// the load. Drives whose index is in `new_drive_indices` (Rust
/// `initial_guest_drive_mounts`) are absent from the source state: they are
/// returned unbound, to be bound and `PATCH`ed after the load.
core::Expected<std::vector<VolumeSlotBind>, std::string> PrepareResumeDrives(
    const std::string& work_dir, const ResumeDrivePlan& plan, const CommonConfig& common,
    const std::vector<std::size_t>& new_drive_indices, FreshDriveResources* resources);

/// What a resume start needs, alongside the capture it restores.
///
/// Separate from `FreshStartSpec` because the two differ in what they send the
/// VMM, not only in their inputs: a resume has no boot source, and instead
/// carries the state file and the memory device the load reads from.
struct ResumeStartSpec {
    /// The capture to restore, already prepared for this launch.
    SnapshotConfig snapshot_config;
    std::string firecracker_bin;
    /// Parent for the per-instance work directory.
    std::string chroot_base_dir;
    core::SandboxId sandbox_id;
    core::Optional<std::string> firecracker_work_dir;
    bool capture_output = true;
    int64_t socket_timeout_ms = 10 * 1000;
    int64_t socket_poll_ms = 10;
    int64_t envd_timeout_ms = 60 * 1000;
    int64_t envd_poll_ms = 100;

    /// Host path of the block device backing guest memory, which the snapshot
    /// load reads through.
    ///
    /// Rust gets this from `UblkDeviceManager::get_or_create_shared_mem`. It
    /// is a parameter here because this port has no ublk daemon: without one
    /// the resume cannot proceed, and an empty value is what says so.
    std::string mem_device_path;

    /// Rust `initial_guest_drive_mounts` — `(index into extra_drives, drive)`
    /// for drives this launch adds that the source state does not contain.
    /// Empty for a paused-state resume, whose drives are all in the state.
    std::vector<std::pair<std::size_t, ExtraDrive> > initial_guest_drive_mounts;

    /// Host-side drive materialization. Absent only for callers that stage the
    /// work directory themselves (tests); the factory sets it whenever it has
    /// a ublk device manager, because Rust's `start_resume` always runs it.
    core::Optional<ResumeDrivePlan> drives;

    /// Rust `NetworkManager::global()`. When set, a slot is allocated and the
    /// VMM is spawned into its namespace, so the load's `eth0 -> tap0`
    /// override finds the tap. NULL means no network (tests). The egress
    /// policy is the capture's own, `snapshot_config.common.network_policy`.
    network::SlotNetworkManager* network_manager = NULL;

    /// Rust `FirecrackerPool::global()`. When set and this launch uses the
    /// default log paths, a pre-spawned VMM and its slot are adopted
    /// (`adopt_warm_firecracker`) instead of spawning one. NULL disables it.
    FirecrackerPool* firecracker_pool = NULL;
};

/// Rust `FirecrackerSandbox::start_resume` — the ordered API sequence a resume
/// issues, as a plan.
///
/// Modelled like `BuildConfigureSequence` and for the same reason: the order
/// is the substance, and a wrong one fails deep inside a load where the error
/// names the last call rather than the cause. Three orderings here are
/// load-bearing:
///
///   * The logger is configured *before* the load. Firecracker accepts
///     `PUT /logger` pre-boot only, and a loaded snapshot is no longer
///     pre-boot.
///
///   * The rate limiter is reconciled *after* the load and *before* the
///     resume. A restored VM inherits whatever limiter was active when it was
///     paused; reconciling after the resume lets the guest issue unthrottled
///     I/O first, and before the load there is no drive to patch.
///
///   * The MMDS document is written before the resume, so the guest cannot
///     read its own identity before it has been corrected.
struct ResumeStep {
    enum class Kind {
        Logger,          ///< `PUT /logger`
        SnapshotLoad,    ///< `PUT /snapshot/load`
        PatchDrivePath,  ///< `PATCH /drives/{id}` — a new volume's path
        Mmds,            ///< `PUT /mmds`
        PatchRateLimiter,///< `PATCH /drives/{id}` — the reconciled limiter
        Resume,          ///< `PATCH /vm {"state":"Resumed"}`
    };

    Kind kind = Kind::Resume;
    /// `PatchDrivePath` / `PatchRateLimiter`: the drive id.
    std::string id;
    /// `Logger`: log path. `SnapshotLoad`: vm state path.
    /// `PatchDrivePath`: the reserved slot's attachment path (relative to the
    /// work dir), which Firecracker reopens.
    std::string path;
    /// `Logger`: level. `SnapshotLoad`: the memory device path.
    /// `PatchDrivePath`: the volume attachment the slot is bound to just
    /// before the PATCH (Rust `bind_volume_drive_slot`); empty = no bind.
    std::string text;
    /// `SnapshotLoad`.
    bool track_dirty_pages = false;
    /// `PatchRateLimiter`.
    RateLimiter rate_limiter;
};

/// Rust `start_resume` — the ordered sequence, derived from the capture.
///
/// Fails rather than producing a partial sequence when the capture is missing
/// what a load needs (rootfs config, rootfs virtual size, memory device). All
/// three are checked before anything is created, because each one would
/// otherwise surface as a VM that loaded and then could not find its disk.
core::Expected<std::vector<ResumeStep>, std::string>
    BuildResumeSequence(const ResumeStartSpec& spec);

/// Issues an already-ordered resume sequence against a live instance.
///
/// Split from `BuildResumeSequence` for the same reason the configure sequence
/// is: the ordering stays testable without a VMM, and the dispatch stays
/// testable without re-deriving the order.
core::Expected<core::Unit, std::string>
    ApplyResumeSequence(FirecrackerInstance* instance,
                        const std::vector<ResumeStep>& steps);

/// Rust `FirecrackerSandbox::start_fresh` — the process half of a fresh boot.
///
/// Spawns the VMM, waits for its API socket, applies `spec.configure` and
/// issues `InstanceStart`. Returns the live instance; on any failure the VMM is
/// stopped before returning, so a caller never receives an error alongside a
/// running Firecracker.
///
/// Shared by `FirecrackerSandboxBackend::StartNowait` and the legacy
/// `FirecrackerBackend::Boot`, which previously each had their own copy of this
/// ordering. Two copies of a sequence whose order is the whole point is exactly
/// how the two drift.
/// The started instance is returned through `out` rather than in the
/// `Expected`: this port's `Expected` unconditionally instantiates its value
/// type's copy constructor, which a `unique_ptr` does not have. Ownership is
/// still all-or-nothing — `out` is only assigned on success.
///
/// When `spec.drives` is set, `resources` is required: it receives the ublk
/// devices the start created and must outlive the VMM. On failure the devices
/// are released before returning.
core::Expected<core::Unit, std::string>
    StartFreshInstance(const FreshStartSpec& spec, std::shared_ptr<Connector> connector,
                       std::unique_ptr<FirecrackerInstance>* out,
                       FreshDriveResources* resources = NULL);

/// Rust `FirecrackerSandbox::start_resume` — the process half of a resume.
///
/// Same shape as `StartFreshInstance` (build the plan, make the work dir,
/// spawn, wait for the socket, apply) with one difference that matters: there
/// is no `InstanceStart`. The VM comes into existence already running from the
/// loaded state, and the sequence's own last step is the resume.
///
/// When `spec.drives` is set, `resources` is required, as on the fresh path:
/// the drives are materialized before the VMM is spawned and released again
/// on failure.
core::Expected<core::Unit, std::string>
    StartResumeInstance(const ResumeStartSpec& spec, std::shared_ptr<Connector> connector,
                        std::unique_ptr<FirecrackerInstance>* out,
                        FreshDriveResources* resources = NULL);

/// Rust `impl SandboxBackend for FirecrackerSandbox`.
///
/// Owns one live microVM. Only the lifecycle methods this port can honour are
/// implemented; the rest report that they are not available in this build
/// rather than silently succeeding, because a caller that believes a snapshot
/// was taken will later resume from a capture that does not exist.
class FirecrackerSandboxBackend : public SandboxBackend {
 public:
    /// Takes ownership of an instance that has already been booted.
    ///
    /// `snapshot_config` is the capture descriptor handed back by `Pause`. The
    /// paused state carries it verbatim, so a resume on another node reads the
    /// same fields this node wrote.
    FirecrackerSandboxBackend(std::unique_ptr<FirecrackerInstance> instance,
                              SnapshotConfig snapshot_config);

    /// A backend that has not started yet, as Rust's `FirecrackerSandbox::new`
    /// returns: the object and its work directory exist, Firecracker does not.
    ///
    /// `connector` may be null, in which case the real unix-socket connector
    /// is built at start time.
    FirecrackerSandboxBackend(FreshStartSpec spec, SnapshotConfig snapshot_config,
                              std::shared_ptr<Connector> connector);

    /// A backend that resumes from a capture, as Rust's
    /// `from_snapshot_config_with_override` returns.
    ///
    /// Distinct from the fresh constructor because `StartNowait` dispatches on
    /// the launch mode and the two send the VMM different things: a resume
    /// issues `PUT /snapshot/load` against drives already bound to the
    /// captured artifacts, where a fresh boot sets a boot source and attaches
    /// drives. Collapsing them would mean a resume silently taking the fresh
    /// path and booting a new VM over a capture nobody loaded.
    ///
    /// `snapshot_config` is expected to have been through
    /// `PrepareSnapshotConfigForResume`, which is where the drive counts are
    /// checked and the runtime identity overridden.
    static FirecrackerSandboxBackend* NewResuming(
        ResumeStartSpec spec, std::shared_ptr<Connector> connector);

    /// Injects the envd health probe. Null uses the real one, which this port
    /// does not have — so without an injected probe `WaitForReady` reports
    /// that it cannot confirm readiness rather than assuming it.
    void SetEnvdProbeForTesting(const EnvdInstance::HealthProbe& probe);
    ~FirecrackerSandboxBackend() override;

    FirecrackerSandboxBackend(const FirecrackerSandboxBackend&) = delete;
    FirecrackerSandboxBackend& operator=(const FirecrackerSandboxBackend&) = delete;

    // ---- implemented ------------------------------------------------------

    /// Rust `start` — `start_nowait` then `wait_for_ready`.
    ///
    /// The split is not cosmetic: `start_nowait` returns once the VM has been
    /// told to run, while `wait_for_ready` waits for **envd inside the guest**
    /// to answer. A caller that submits work after only `start_nowait` is
    /// talking to a VM whose agent may not exist yet.
    core::Expected<core::Unit, core::AnyError> Start() override;

    /// Rust `start_nowait` — dispatches on `LaunchMode` and, for `Fresh`,
    /// runs the spawn/configure/start sequence.
    ///
    /// Refuses a second start rather than spawning another VMM over the first,
    /// which would leak the original and leave two processes on one socket
    /// path.
    core::Expected<core::Unit, core::AnyError> StartNowait() override;

    /// Rust `wait_for_ready` — waits for envd, not for the API socket.
    ///
    /// The API socket is waited on inside `StartNowait`; this is the readiness
    /// the *workload* depends on. Rust errors when no envd instance exists,
    /// and so does this: reporting readiness without having confirmed it is
    /// the one answer that cannot be recovered from.
    core::Expected<core::Unit, core::AnyError> WaitForReady() override;

    /// Rust `envd` — the stored instance, which exists only once
    /// `WaitForReady` has observed a healthy guest.
    ///
    /// Nothing before that: a handle to an agent that is not answering *yet*
    /// is indistinguishable, to the caller, from one that has stopped
    /// answering.
    std::shared_ptr<EnvdApi> Envd() const override;

    /// Rust `pause` — `PATCH /vm {"state":"Paused"}`, then wrap the capture
    /// descriptor in a `FirecrackerPausedState`.
    ///
    /// On failure this runs Rust's `recover_capture_failure`: a recoverable
    /// capture error is reported only after the VM has been put back into a
    /// running state, and a resume that itself fails upgrades the error to
    /// `Terminal`. The two variants are not interchangeable — `Recoverable`
    /// promises the caller the sandbox is still runnable, so returning it
    /// without having resumed would strand a paused VM that everyone believes
    /// is live.
    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> >
        Pause(const core::Optional<std::string>& artifact_root) override;

    /// Rust `pause_to_dir` — pause, then write the capture artifacts into
    /// `spec.snapshot_dir`. See [`PauseToDirSpec`] for the ordering rules.
    ///
    /// Reports through `SandboxCaptureError` like `Pause` does, and for the
    /// same reason: a caller needs to know whether the runtime was mutated.
    /// Unlike `Pause`, a failure here has usually already paused the VM, so
    /// the recovery resume matters more, not less.
    SandboxCaptureResult<PauseToDirResult> PauseToDir(const PauseToDirSpec& spec);

    /// Rust `resume` — `PATCH /vm {"state":"Resumed"}`.
    ///
    /// Idempotent, as the trait requires. The idempotence is Firecracker's:
    /// resuming an already-running VM is accepted, so Rust forwards the call
    /// unconditionally and this does too. Tracking a local "is paused" flag
    /// and skipping the call would be worse than useless — the flag can only
    /// disagree with the VMM, and the disagreement surfaces as a sandbox that
    /// silently stays paused.
    core::Expected<core::Unit, core::AnyError> Resume() override;

    /// Rust `stop` — idempotent. Stops the VMM and removes its API socket.
    core::Expected<core::Unit, core::AnyError> Stop() override;

    /// Rust `snapshot` — `pause_to_dir` into a managed directory, then resume.
    /// Returns the directory holding the capture.
    ///
    /// Unlike `Pause`, the sandbox keeps running afterwards, which is the
    /// whole difference between the two: a snapshot is taken *of* a live
    /// sandbox, so the resume is part of the operation rather than the
    /// caller's next step. A resume that fails is therefore `Terminal` even
    /// though the capture itself succeeded — the artifacts are fine, the
    /// sandbox is not.
    ///
    /// The destination is `{ManagedSnapshotBase}/{sandbox_id}/{uuid}`. The
    /// per-sandbox root is resolved once and reused, so repeated snapshots of
    /// one sandbox stay together instead of scattering across the base.
    SandboxCaptureResult<std::string> Snapshot() override;

    /// Rust `runtime_info`.
    SandboxRuntimeInfo RuntimeInfoOf() const override;

    /// Overrides the memory-layer write used by `Snapshot`, which otherwise
    /// reads guest memory out of the live VMM with `process_vm_readv`.
    ///
    /// Without this the capture path cannot be exercised at all off a real
    /// microVM: a stand-in VMM has no guest memory to read, so the layer write
    /// fails for a reason that has nothing to do with the sequence under test.
    void SetMemoryLayerWriterForTesting(const MemoryLayerWriter& writer);

    // ---- volume capture: no-op when there is nothing to capture ------------
    //
    // Each of these is driven by the set of *writable persistent volumes*
    // (writable and carrying a snapshot output dir). Upstream they are no-ops
    // when that set is empty, which this reproduces -- a sandbox with no such
    // volume is the common case in this build, and refusing it would refuse a
    // capture Rust completes.
    //
    // With such a volume present they report an error instead of succeeding:
    // sealing one needs the overlaybd layer writer (absent here) and freezing
    // one needs an envd client to run the ioctl in the guest (also absent).
    // Claiming success would publish a volume whose snapshot layer was never
    // written, and that only surfaces at a later resume on another node.

    SandboxCaptureResult<core::Unit> SnapshotVolumes() override;
    SandboxCaptureResult<core::Unit> FreezeAndSnapshotVolumes() override;
    /// Always succeeds: nothing here ever freezes, so no thaw is ever owed.
    core::Expected<core::Unit, core::AnyError> ThawVolumes() override;

    // ---- not available in this build --------------------------------------
    //
    // Reported as errors rather than as success. A caller that is told a
    // snapshot succeeded will later try to resume from a capture that was
    // never written, and the failure then appears on a different node, at a
    // different time, as a corrupt record.

    SandboxCaptureResult<std::vector<SandboxForkResult> >
        Fork(const std::vector<SandboxForkSpec>& specs) override;
    core::Optional<std::string> HostInteractionIp() const override;
    RuntimeArtifactSet StartupArtifacts() const override;
    core::Expected<core::Unit, core::AnyError>
        UpdateNetworkPolicy(const core::Optional<std::string>& policy) override;
    void UpdateCustomExtensionParams(const core::Optional<std::string>& params) override;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_SANDBOX_BACKEND_H_
