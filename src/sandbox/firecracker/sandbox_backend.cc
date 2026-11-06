// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — `impl SandboxBackend for
// FirecrackerSandbox`, plus `recover_capture_failure`.
#include <unistd.h>
#include <fcntl.h>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>
#include "agentenv/sandbox/firecracker/sandbox_backend.h"
#include "agentenv/sandbox/network.h"
#include "agentenv/sandbox/network/manager.h"
#include "agentenv/sandbox/network/slot.h"
#include "agentenv/sandbox/firecracker/pool.h"
#include "agentenv/core/fs.h"

#include "agentenv/core/logging.h"
#include "agentenv/sandbox/firecracker/factory.h"
#include "agentenv/sandbox/firecracker/process_vm.h"

#include "agentenv/sandbox/firecracker/lifecycle.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

/// Rust `impl From<anyhow::Error> for SandboxCaptureError`.
///
/// An error that is not already classified becomes `Recoverable`. The default
/// direction matters: `Terminal` tells the orchestrator the runtime was
/// mutated and must not be trusted, so defaulting there would condemn sandboxes
/// over transient faults. `Recoverable` is only safe to report once the VM is
/// actually running again, which is what `RecoverCaptureFailure` enforces.
SandboxCaptureError ClassifyCaptureError(const std::string& message) {
    return SandboxCaptureError::Recoverable(message);
}

}  // namespace

std::string ManagedSnapshotBase(const core::Optional<std::string>& work_dir) {
    // Rust `std::env::temp_dir()`: `$TMPDIR` when set, `/tmp` otherwise. The
    // `aenv` component only appears on the fallback path, matching Rust —
    // a configured work_dir is already ours, so it is not namespaced again.
    if (work_dir.has_value() && !work_dir->empty()) {
        return *work_dir + "/managed-snapshots";
    }
    const char* tmp = ::getenv("TMPDIR");
    std::string base = (tmp != NULL && *tmp != '\0') ? std::string(tmp) : std::string("/tmp");
    while (base.size() > 1 && base[base.size() - 1] == '/') base.erase(base.size() - 1);
    return base + "/aenv/managed-snapshots";
}

/// Rust: the `ProcessVmReader` element of `src_layers`.
///
/// `CompactTo` addresses sources by byte offset; for this layer that offset is
/// a **host virtual address** in the VMM, not a file position. The mappings
/// were built from `base_host_virt_addr`, so the two agree by construction --
/// but it is why there is only ever one source layer here (tag 0): a second
/// layer would need its own address space to resolve against.
class ProcessVmCompactSource : public storage::overlaybd::lsmt::CompactSource {
 public:
    explicit ProcessVmCompactSource(int64_t pid) : reader_(pid) {}

    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) override {
        if (tag != 0) return false;
        // A short read must fail rather than leave the tail as zeros: zeroed
        // guest memory is indistinguishable from memory that was never read,
        // so a partial read would corrupt the snapshot silently.
        return reader_.ReadExactRemote(src_byte_off, dst, static_cast<std::size_t>(len)).ok();
    }

 private:
    ProcessVmReader reader_;
};

/// Rust `tokio::fs::create_dir_all`.
core::Expected<core::Unit, std::string> MakeDirs(const std::string& path) {
    std::string acc;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty() && ::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
                return core::make_unexpected(std::string("create ") + acc + ": " +
                                             std::strerror(errno));
            }
        }
        if (i < path.size()) acc.push_back(path[i]);
    }
    return core::Unit{};
}

/// Rust `cleanup_failed_snapshot_dir`.
///
/// Best-effort and deliberately silent about a missing directory: the capture
/// may have failed before the mkdir, and Rust treats `NotFound` as success.
/// Any other failure is logged, not propagated — the capture error is what the
/// caller needs, and replacing it with a cleanup error would hide the cause.
void CleanupFailedSnapshotDir(const std::string& path) {
    if (path.empty()) return;
    DIR* dir = ::opendir(path.c_str());
    if (dir == NULL) return;  // includes ENOENT, as Rust's NotFound arm does
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;
        struct stat st;
        // lstat, not stat: a symlink inside the directory must be removed
        // rather than followed into whatever it points at.
        if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            CleanupFailedSnapshotDir(child);
        } else {
            ::unlink(child.c_str());
        }
    }
    ::closedir(dir);
    ::rmdir(path.c_str());
}

core::Expected<core::Unit, std::string> PublishMemoryOverlaybdLayer(
    const std::string& output_path,
    const std::vector<storage::overlaybd::lsmt::SegmentMapping>& mappings,
    uint64_t virtual_size, int64_t firecracker_pid) {
    // Rust: `output_path.with_extension("commit.tmp")`. The final name is what
    // the resume path loads, and a reader cannot tell a truncated commit file
    // from a short one, so it must appear only once complete.
    const std::string tmp_path = output_path + ".tmp";

    ProcessVmCompactSource source(firecracker_pid);
    std::string err;
    const bool ok = storage::overlaybd::lsmt::CompactTo(tmp_path, &source, mappings,
                                                        virtual_size, &err);
    if (!ok) {
        ::unlink(tmp_path.c_str());
        return core::make_unexpected(std::string("compact memory layer: ") + err);
    }
    if (::rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        ::unlink(tmp_path.c_str());
        return core::make_unexpected(
            std::string("move sealed memory lower into place failed: ") + output_path +
            ": " + why);
    }
    return core::Unit{};
}

// ── fresh-start host drives (Rust start_fresh / extra_drive.rs) ─────────────

const char* const kGuestArpNotifyBootArg = "sysctl.net.ipv4.conf.all.arp_notify=1";

core::Expected<core::Unit, std::string> FreshDriveResources::Release() {
    // Rust `SandboxResources::release`: rootfs, then extra drives, then the
    // network slot. Every device is attempted even after one fails, because
    // each one left behind is a leaked `/dev/ublkbN` with no handle that
    // could ever free it.
    if (!manager_) {
        has_rootfs_ = false;
        extra_drives_.clear();
    } else if (has_rootfs_) {
        const ublk::daemon::DaemonResult<core::Unit> r = manager_->ReleaseDevice(rootfs_.device);
        if (!r.ok()) {
            AGENTENV_WARN("failed to release ublk device during stop: " << r.error().message);
        }
        has_rootfs_ = false;
    }
    for (std::size_t i = 0; manager_ && i < extra_drives_.size(); ++i) {
        const ublk::daemon::DaemonResult<core::Unit> r =
            manager_->ReleaseDevice(extra_drives_[i].device);
        if (!r.ok()) {
            AGENTENV_WARN("failed to release extra drive device during stop: "
                          << r.error().message);
        }
    }
    extra_drives_.clear();

    // Rust `self.tools_ublk_device.take()`. Dropped, not released: the device
    // is shared, so this hands back only *this* sandbox's share and the
    // manager decides whether any holder is left. Calling `ReleaseDevice` on
    // it instead would tear down a device other running sandboxes are still
    // reading their tools drive from. Ordered after the per-sandbox devices
    // for the same reason they come after the VMM stop — nothing may be
    // removed from under a guest that is still running.
    tools_ = ublk::SharedReadOnlyDevice();

    // Taken before the call, as Rust's `network_slot.take()`: the manager owns
    // the slot from here whether it pools it or tears it down, so a retry must
    // not hand it back a second time.
    network::Slot* slot = network_slot_;
    network::SlotNetworkManager* manager = network_manager_;
    network_slot_ = NULL;
    network_manager_ = NULL;
    if (slot != NULL && manager != NULL) {
        const uint32_t idx = slot->Idx();
        const core::Expected<core::Unit, std::string> r = manager->Release(slot);
        if (!r.ok()) {
            return core::make_unexpected("Failed to release network slot: " + r.error());
        }
        AGENTENV_DEBUG("network slot released: " << idx);
    }
    return core::Unit{};
}

void AppendBootArg(core::Optional<std::string>* boot_args, const std::string& arg) {
    if (boot_args->has_value()) {
        *boot_args = core::Optional<std::string>(**boot_args + " " + arg);
    } else {
        *boot_args = core::Optional<std::string>(arg);
    }
}

core::Optional<std::string> ComposeFreshBootArgs(const core::Optional<std::string>& boot_args,
                                                 const std::vector<ExtraDrive>& extra_drives,
                                                 uint32_t mem_size_mib,
                                                 const std::string& arch) {
    return ComposeFreshBootArgs(boot_args, extra_drives, mem_size_mib, arch,
                                std::vector<std::string>());
}

core::Optional<std::string> ComposeFreshBootArgs(const core::Optional<std::string>& boot_args,
                                                 const std::vector<ExtraDrive>& extra_drives,
                                                 uint32_t mem_size_mib,
                                                 const std::string& arch,
                                                 const std::vector<std::string>& network_args) {
    core::Optional<std::string> out = boot_args;

    // The tools drive carries /init; an operator-supplied `init=` wins.
    bool has_init = false;
    if (out.has_value()) {
        const std::string& s = *out;
        std::size_t i = 0;
        while (i < s.size() && !has_init) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            const std::size_t start = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (s.compare(start, 5, "init=") == 0 && i - start >= 5) has_init = true;
        }
    }
    if (!has_init) AppendBootArg(&out, "init=/init");

    // `agentenv_drives=vdc:<mount>[:<sub>],...` -- the guest's mount table.
    const core::Optional<std::string> drives = BuildDrivesBootArg(extra_drives);
    if (drives.has_value()) AppendBootArg(&out, *drives);

    // Rust: `slot.build_ip_boot_arg()` then `GUEST_ARP_NOTIFY_BOOT_ARG`,
    // after the drives and before the DAMON region.
    for (std::size_t i = 0; i < network_args.size(); ++i) AppendBootArg(&out, network_args[i]);

    return AddDamonMonitorRegion(out, mem_size_mib, arch);
}

core::Expected<std::vector<DriveMount>, std::string> CreateVolumeDriveSlots(
    const std::string& work_dir, std::size_t physical_extra_drive_count, std::size_t count) {
    // Rust checks this after creating the files; checking first leaves
    // nothing behind for a request that can never be satisfied.
    if (physical_extra_drive_count + count > kMaxExtraDrives) {
        return core::make_unexpected(
            std::to_string(physical_extra_drive_count) + " physical extra drives plus " +
            std::to_string(count) + " volume slots exceed Firecracker's " +
            std::to_string(kMaxExtraDrives) + "-drive limit");
    }
    std::vector<DriveMount> slots;
    slots.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        DriveMount slot;
        slot.drive_id = VolumeDriveSlotId(i);
        slot.attachment_path = slot.drive_id + ".img";
        slot.read_only = false;
        const std::string path = work_dir + "/" + slot.attachment_path;
        // create_new: a leftover file would be attached as if it were ours.
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return core::make_unexpected("create reserved volume drive " + path + ": " +
                                         std::strerror(errno));
        }
        const int truncated = ::ftruncate(fd, static_cast<off_t>(kVolumeDrivePlaceholderSize));
        const int saved = errno;
        ::close(fd);
        if (truncated != 0) {
            return core::make_unexpected("size reserved volume drive " + path + ": " +
                                         std::strerror(saved));
        }
        slots.push_back(slot);
    }
    return slots;
}

core::Expected<core::Unit, std::string> BindVolumeDriveSlot(const std::string& work_dir,
                                                            const DriveMount& slot,
                                                            const DriveMount& drive) {
    const std::string slot_path = work_dir + "/" + slot.attachment_path;
    if (::unlink(slot_path.c_str()) != 0) {
        return core::make_unexpected("remove reserved volume drive " + slot_path + ": " +
                                     std::strerror(errno));
    }
    // Relative target, as Rust links `drive.attachment_path`: it resolves
    // inside the work directory, next to the slot itself.
    if (::symlink(drive.attachment_path.c_str(), slot_path.c_str()) != 0) {
        return core::make_unexpected("link reserved volume drive " + slot_path + " to " +
                                     drive.attachment_path + ": " + std::strerror(errno));
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string> PrepareVolumeDriveSlots(
    const std::string& work_dir, std::size_t count,
    const std::vector<DriveMount>& volume_attachments) {
    if (volume_attachments.size() > count) {
        return core::make_unexpected(
            std::string("fresh sandbox has more volume drives than reserved volume slots"));
    }
    const core::Expected<std::vector<DriveMount>, std::string> slots =
        CreateVolumeDriveSlots(work_dir, 0, count);
    if (!slots.ok()) return core::make_unexpected(slots.error());
    for (std::size_t i = 0; i < volume_attachments.size(); ++i) {
        const core::Expected<core::Unit, std::string> bound =
            BindVolumeDriveSlot(work_dir, slots.value()[i], volume_attachments[i]);
        if (!bound.ok()) return core::make_unexpected(bound.error());
    }
    return core::Unit{};
}

namespace {

/// Rust `std::os::unix::fs::symlink` with its error context.
core::Expected<core::Unit, std::string> Symlink(const std::string& target,
                                                const std::string& link,
                                                const std::string& what) {
    if (::symlink(target.c_str(), link.c_str()) != 0) {
        return core::make_unexpected("symlink " + what + " " + link + " -> " + target + ": " +
                                     std::strerror(errno));
    }
    return core::Unit{};
}

/// What `PrepareHostDrives` needs; the fresh and resume paths differ only in
/// the rootfs sizes and in the per-drive prepare mode.
struct HostDriveRequest {
    std::shared_ptr<ublk::DeviceManager> device_manager;
    std::string tools_drive_path;
    std::string tools_drive_version;
    ToolsDriveResolver tools_drive_resolver;
    /// NULL: no user rootfs (Rust resume skips it without a `ublk_config`).
    const ublk::OverlaybdDeviceConfig* rootfs = NULL;
    core::Optional<uint64_t> rootfs_requested_size;
    core::Optional<uint64_t> rootfs_known_size;
    bool rootfs_allow_shrink = false;
    const std::vector<ExtraDrive>* extra_drives = NULL;
    std::string overlaybd_global_config;
    storage::overlaybd::UpperMode runtime_upper_mode =
        storage::overlaybd::UpperMode::HybridLogStructured;
    /// Rust's `mode: impl Fn(usize) -> ExtraDrivePrepareMode`.
    std::function<ExtraDrivePrepareMode(std::size_t)> mode;
};

/// Rust `link_tools_drive` + `attach_user_rootfs` + `prepare_extra_drives`.
core::Expected<std::vector<DriveMount>, std::string> PrepareHostDrives(
    const std::string& work_dir, const HostDriveRequest& req, FreshDriveResources* resources) {
    if (resources == NULL) return core::make_unexpected(std::string("no resource holder"));

    if (!req.device_manager || !req.device_manager->is_available()) {
        return core::make_unexpected(std::string("ublk daemon client is unavailable"));
    }
    resources->Adopt(req.device_manager);

    // ── Tools drive (Rust `prepare_tools_drive` then `link_tools_drive`) ──
    //
    // Two shapes, and the choice is made here rather than when the plan was
    // built, because the shared device's lifetime has to be the sandbox's: the
    // handle is adopted into `resources` in the same step that attaches it, so
    // a failure after this point still releases it and a successful start
    // holds it until teardown. Deciding at plan time would create the device
    // with nobody holding it.
    std::string tools_drive_path = req.tools_drive_path;
    if (req.tools_drive_resolver) {
        const core::Expected<core::Optional<std::string>, std::string> prepared =
            req.tools_drive_resolver(req.tools_drive_version);
        if (!prepared.ok()) {
            return core::make_unexpected("prepare tools drive version '" +
                                         req.tools_drive_version + "': " + prepared.error());
        }
        if (prepared.value().has_value()) {
            ublk::CreateSpec spec;
            spec.image_config = *prepared.value();
            spec.global_config = req.overlaybd_global_config;
            const ublk::daemon::DaemonResult<ublk::SharedReadOnlyDevice> shared =
                req.device_manager->GetOrCreateSharedTools(spec);
            if (!shared.ok()) {
                return core::make_unexpected("create shared tools device for version '" +
                                             req.tools_drive_version + "': " +
                                             shared.error().message);
            }
            resources->AdoptSharedTools(shared.value());
            tools_drive_path = shared.value().device_path();
        }
        // An empty optional is the legacy ext4 drive, already in
        // `tools_drive_path` — not an error, and not a reason to skip the
        // attach below.
    }

    // Canonicalized: a dangling path must fail here, not as a guest with no
    // root filesystem. Applies to both shapes, since a ublk device node that
    // the daemon reported but did not create would fail the same way.
    {
        char resolved[PATH_MAX];
        if (::realpath(tools_drive_path.c_str(), resolved) == NULL) {
            return core::make_unexpected("open tools drive at " + tools_drive_path + ": " +
                                         std::strerror(errno));
        }
        const core::Expected<core::Unit, std::string> linked =
            Symlink(resolved, work_dir + "/" + kRootfsDrivePath, "tools drive");
        if (!linked.ok()) return core::make_unexpected(linked.error());
    }

    // ── User image (Rust `attach_user_rootfs`) ──
    if (req.rootfs != NULL) {
        const ublk::daemon::DaemonResult<ublk::OverlaybdRuntimeDevice> created =
            req.device_manager->CreateOverlaybdRuntimeDevice(
                req.rootfs->image_config_path, req.overlaybd_global_config,
                work_dir + "/overlaybd", req.rootfs->read_only,
                req.rootfs->runtime_upper_mode, req.rootfs_requested_size,
                req.rootfs_known_size, req.rootfs_allow_shrink);
        if (!created.ok()) {
            return core::make_unexpected("create user image overlaybd runtime device: " +
                                         created.error().message);
        }
        // Recorded before the symlink, so a symlink failure still releases it.
        resources->SetRootfs(created.value());
        const core::Expected<core::Unit, std::string> linked =
            Symlink(created.value().device.device_path, work_dir + "/" + kUserRootfsDrivePath,
                    "user-rootfs to ublk device");
        if (!linked.ok()) return core::make_unexpected(linked.error());
    }

    // ── Extra drives (Rust `prepare_extra_drives`) ──
    const std::vector<ExtraDrive> no_drives;
    const std::vector<ExtraDrive>& extra_drives =
        req.extra_drives != NULL ? *req.extra_drives : no_drives;
    std::vector<DriveMount> mounts;
    std::vector<std::string> made_links;
    mounts.reserve(extra_drives.size());
    for (std::size_t i = 0; i < extra_drives.size(); ++i) {
        const ExtraDrive& drive = extra_drives[i];
        const ExtraDrivePrepareMode mode =
            req.mode ? req.mode(i) : ExtraDrivePrepareMode::MakeResume();
        core::Optional<uint64_t> target_size;
        core::Optional<uint64_t> base_size;
        mode.DeviceSizes(drive, &target_size, &base_size);

        const ublk::daemon::DaemonResult<ublk::OverlaybdRuntimeDevice> created =
            req.device_manager->CreateOverlaybdRuntimeDevice(
                drive.image_config_path, req.overlaybd_global_config,
                drive.RuntimeDir(work_dir), drive.read_only, req.runtime_upper_mode,
                target_size, base_size, mode.AllowShrink());
        std::string failure;
        if (!created.ok()) {
            failure = "prepare extra drives: create overlaybd extra drive runtime device: " +
                      created.error().message;
        } else {
            resources->AddExtraDrive(created.value());
            const std::string name = drive.AttachmentSymlinkName();
            const std::string link = work_dir + "/" + name;
            const core::Expected<core::Unit, std::string> linked =
                Symlink(created.value().device.device_path, link, "extra drive");
            if (!linked.ok()) {
                failure = "prepare extra drives: " + linked.error();
            } else {
                made_links.push_back(link);
                DriveMount mount;
                mount.drive_id = drive.drive_id;
                mount.attachment_path = name;
                mount.read_only = drive.read_only;
                mounts.push_back(mount);
            }
        }
        if (!failure.empty()) {
            // Rust `PreparedDrives::cleanup`: the attachments made so far are
            // removed; the devices go with the caller's resource release.
            for (std::size_t k = 0; k < made_links.size(); ++k) {
                if (::unlink(made_links[k].c_str()) != 0 && errno != ENOENT) {
                    AGENTENV_WARN("failed to remove prepared extra drive attachment "
                                  << made_links[k] << " during rollback: "
                                  << std::strerror(errno));
                }
            }
            return core::make_unexpected(failure);
        }
    }
    return mounts;
}

bool ContainsIndex(const std::vector<std::size_t>& indices, std::size_t index) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == index) return true;
    }
    return false;
}

}  // namespace

core::Expected<std::vector<DriveMount>, std::string> PrepareFreshDrives(
    const std::string& work_dir, const FreshDrivePlan& plan, FreshDriveResources* resources) {
    HostDriveRequest req;
    req.device_manager = plan.device_manager;
    req.tools_drive_path = plan.tools_drive_path;
    req.tools_drive_version = plan.tools_drive_version;
    req.tools_drive_resolver = plan.tools_drive_resolver;
    req.rootfs = &plan.rootfs_image_config;
    req.rootfs_requested_size = plan.rootfs_virtual_size;
    req.rootfs_allow_shrink = plan.rootfs_allow_shrink;
    req.extra_drives = &plan.extra_drives;
    req.overlaybd_global_config = plan.overlaybd_global_config;
    req.runtime_upper_mode = plan.runtime_upper_mode;
    const ExtraDrivePrepareMode fresh =
        ExtraDrivePrepareMode::MakeFresh(plan.extra_drive_allow_shrink);
    req.mode = [fresh](std::size_t) -> ExtraDrivePrepareMode { return fresh; };
    return PrepareHostDrives(work_dir, req, resources);
}

core::Expected<std::vector<VolumeSlotBind>, std::string> PrepareResumeDrives(
    const std::string& work_dir, const ResumeDrivePlan& plan, const CommonConfig& common,
    const std::vector<std::size_t>& new_drive_indices, FreshDriveResources* resources) {
    // Rust `start_resume` checks both before attaching anything.
    if (!common.rootfs_virtual_size.has_value()) {
        return core::make_unexpected(std::string("snapshot rootfs virtual size is missing"));
    }
    if (!common.rootfs_image_config.has_value()) {
        return core::make_unexpected(std::string("snapshot rootfs image config is missing"));
    }

    HostDriveRequest req;
    req.device_manager = plan.device_manager;
    req.tools_drive_path = plan.tools_drive_path;
    req.tools_drive_version = plan.tools_drive_version;
    req.tools_drive_resolver = plan.tools_drive_resolver;
    // Rust `if config.common.ublk_config.is_some() { attach_user_rootfs(..,
    // Some(size), Some(size), false) }`: the recorded size is both the target
    // and the base, and a restored disk is never shrunk.
    if (common.ublk_config.has_value()) {
        req.rootfs = &*common.rootfs_image_config;
        req.rootfs_requested_size = common.rootfs_virtual_size;
        req.rootfs_known_size = common.rootfs_virtual_size;
        req.rootfs_allow_shrink = false;
    }
    req.extra_drives = &common.extra_drives;
    req.overlaybd_global_config = plan.overlaybd_global_config;
    req.runtime_upper_mode = plan.runtime_upper_mode;
    // Launch-slot drives new to this launch carry the requested size, as on a
    // fresh boot; every other drive, volumes included, carries its recorded
    // size. This port has no `ExtraDriveKind::LaunchSlot`: a non-volume drive
    // among the new ones is one, since every launch drive uses a slot.
    const std::vector<ExtraDrive>& drives = common.extra_drives;
    const bool allow_shrink = plan.extra_drive_allow_shrink;
    req.mode = [&drives, &new_drive_indices, allow_shrink](std::size_t index) {
        if (ContainsIndex(new_drive_indices, index) && !drives[index].IsVolume()) {
            return ExtraDrivePrepareMode::MakeFresh(allow_shrink);
        }
        return ExtraDrivePrepareMode::MakeResume();
    };
    const core::Expected<std::vector<DriveMount>, std::string> prepared =
        PrepareHostDrives(work_dir, req, resources);
    if (!prepared.ok()) {
        return core::make_unexpected("prepare snapshot-backed extra drives for resume: " +
                                     prepared.error());
    }
    const std::vector<DriveMount>& attachments = prepared.value();

    const std::size_t physical = common.physical_extra_drive_count;
    const core::Expected<std::vector<DriveMount>, std::string> slots =
        CreateVolumeDriveSlots(work_dir, physical, common.volume_drive_slots);
    if (!slots.ok()) {
        return core::make_unexpected("prepare reserved volume drive slots for resume: " +
                                     slots.error());
    }
    if (physical > attachments.size()) {
        return core::make_unexpected(
            "snapshot expects " + std::to_string(physical) +
            " physical extra drives, but only " + std::to_string(attachments.size()) +
            " were prepared");
    }
    if (attachments.size() - physical > slots.value().size()) {
        return core::make_unexpected(
            std::string("snapshot has more launch-time drives than reserved volume slots"));
    }

    std::vector<VolumeSlotBind> new_binds;
    for (std::size_t offset = 0; offset < attachments.size() - physical; ++offset) {
        VolumeSlotBind bind;
        bind.slot = slots.value()[offset];
        bind.drive = attachments[physical + offset];
        if (ContainsIndex(new_drive_indices, physical + offset)) {
            // Absent from the source state: Firecracker loads the placeholder,
            // and the bind plus PATCH happen after the load.
            new_binds.push_back(bind);
            continue;
        }
        // In the source state: bound now, so the load reopens the real device.
        const core::Expected<core::Unit, std::string> bound =
            BindVolumeDriveSlot(work_dir, bind.slot, bind.drive);
        if (!bound.ok()) {
            return core::make_unexpected("bind restored drive " + bind.drive.drive_id +
                                         " to reserved drive " + bind.slot.drive_id +
                                         " before snapshot load: " + bound.error());
        }
    }
    return new_binds;
}

FirecrackerLogPaths ResolveFirecrackerLogPaths(
    const core::Optional<std::string>& serial_output_base_dir,
    const core::Optional<std::string>& stdout_dir, const core::Optional<std::string>& stderr_dir,
    bool capture_output, const core::SandboxId& sandbox_id, const std::string& work_dir) {
    FirecrackerLogPaths paths;
    // Rust `default_log_dir`.
    paths.default_log_dir = serial_output_base_dir.has_value()
                                ? *serial_output_base_dir + "/" + sandbox_id.ToString()
                                : work_dir + "/logs";
    paths.stdout_target =
        (stdout_dir.has_value() ? *stdout_dir : paths.default_log_dir) + "/firecracker-stdout.log";
    paths.stderr_target =
        (stderr_dir.has_value() ? *stderr_dir : paths.default_log_dir) + "/firecracker-stderr.log";
    // Rust `firecracker_stdio_paths`.
    if (capture_output || stdout_dir.has_value()) paths.stdout_path = paths.stdout_target;
    if (capture_output || stderr_dir.has_value()) paths.stderr_path = paths.stderr_target;
    paths.logger_path = paths.default_log_dir + "/firecracker.log";
    return paths;
}

core::Expected<core::Unit, std::string> RelocateWarmLog(const core::Optional<std::string>& src,
                                                       const std::string& target) {
    if (!src.has_value()) return core::Unit();
    if (*src == target || !core::fs::Exists(*src)) return core::Unit();
    const std::string::size_type slash = target.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string parent = target.substr(0, slash);
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(parent);
        if (!made.ok()) {
            return core::make_unexpected("create firecracker log directory " + parent + ": " +
                                         made.error());
        }
    }
    if (core::fs::Exists(target) && ::unlink(target.c_str()) != 0) {
        return core::make_unexpected("remove existing firecracker log " + target + ": " +
                                     std::strerror(errno));
    }
    if (::rename(src->c_str(), target.c_str()) == 0) return core::Unit();
    if (errno != EXDEV) {
        return core::make_unexpected("move warm firecracker log " + *src + " to " + target +
                                     ": " + std::strerror(errno));
    }
    const core::Expected<core::Unit, std::string> copied = core::fs::Copy(*src, target);
    if (!copied.ok()) {
        return core::make_unexpected("copy warm firecracker log " + *src + " to " + target +
                                     " after cross-device rename failed: " + copied.error());
    }
    if (::unlink(src->c_str()) != 0 && errno != ENOENT) {
        return core::make_unexpected("remove warm firecracker log " + *src +
                                     " after cross-device copy: " + std::strerror(errno));
    }
    return core::Unit();
}

namespace {

/// Rust `TempDir` drop for a start that did not hand its work dir over.
struct RemoveWorkDirOnFailure {
    std::string path;
    bool armed;
    ~RemoveWorkDirOnFailure() {
        if (armed && !path.empty()) (void)core::fs::RemoveDirAll(path);
    }
};

/// Rust `adopt_warm_firecracker` — take a pre-spawned Firecracker and its
/// network slot from the warm pool when this launch uses the default log
/// paths. On success the slot is already in `resources`, `*instance` holds
/// the running VMM and `*work_dir` is the entry's work dir.
bool AdoptWarmFirecracker(const ResumeStartSpec& spec, FreshDriveResources* resources,
                          std::unique_ptr<FirecrackerInstance>* instance,
                          network::Slot** slot, std::string* work_dir) {
    const CommonConfig& common = spec.snapshot_config.common;
    if (spec.firecracker_pool == NULL) return false;
    if (common.stdout_path.has_value() || common.stderr_path.has_value()) return false;
    const bool capture_output = LoggingEnabled(common.firecracker_log_level);
    const core::Expected<std::shared_ptr<WarmFirecracker>, std::string> acquired =
        spec.firecracker_pool->AcquireFor(capture_output);
    if (!acquired.ok()) return false;
    const std::shared_ptr<WarmFirecracker>& warm = acquired.value();

    core::Optional<std::string> warm_stdout;
    core::Optional<std::string> warm_stderr;
    WarmStdioPaths(warm->WorkDir(), capture_output, &warm_stdout, &warm_stderr);
    AGENTENV_DEBUG("using warm firecracker from pool slot=" << warm->GetSlot()->Idx()
                                                            << " pool_work_dir="
                                                            << warm->WorkDir());

    resources->AdoptNetworkSlot(spec.firecracker_pool->NetworkManager(), warm->GetSlot());
    *slot = warm->GetSlot();
    // The work dir is switched before relocating logs, since the fallback log
    // paths are relative to it.
    *work_dir = warm->WorkDir();
    *instance = warm->TakeInstance();

    const FirecrackerLogPaths paths =
        ResolveFirecrackerLogPaths(common.serial_output_base_dir, common.stdout_path,
                                   common.stderr_path, capture_output, spec.sandbox_id,
                                   *work_dir);
    const core::Expected<core::Unit, std::string> out =
        RelocateWarmLog(warm_stdout, paths.stdout_target);
    if (!out.ok()) {
        AGENTENV_WARN("failed to relocate warm firecracker stdout log: " << out.error());
    }
    const core::Expected<core::Unit, std::string> err =
        RelocateWarmLog(warm_stderr, paths.stderr_target);
    if (!err.ok()) {
        AGENTENV_WARN("failed to relocate warm firecracker stderr log: " << err.error());
    }
    return true;
}

/// Rust `configure_logger`'s directory step: the logger file is opened by the
/// VMM, so its directory has to exist first.
core::Expected<core::Unit, std::string> PrepareLoggerDir(const FirecrackerLogPaths& paths) {
    const core::Expected<core::Unit, std::string> made =
        core::fs::CreateDirAll(paths.default_log_dir);
    if (!made.ok()) {
        return core::make_unexpected("create firecracker log directory " +
                                     paths.default_log_dir + ": " + made.error());
    }
    return core::Unit();
}

/// Rust `allocate_network_slot` — the slot is stored in `resources` before
/// any fallible setup, so `Stop` or the destructor releases it.
core::Expected<network::Slot*, std::string> AllocateNetworkSlot(
    network::SlotNetworkManager* manager, FreshDriveResources* resources) {
    if (manager == NULL) return core::make_unexpected(std::string("no network manager"));
    const core::Expected<network::Slot*, std::string> slot = manager->AllocateAny();
    if (!slot.ok()) {
        return core::make_unexpected("Failed to allocate network slot: " + slot.error());
    }
    resources->AdoptNetworkSlot(manager, slot.value());
    AGENTENV_DEBUG("allocated network slot " << slot.value()->Idx());
    return slot.value();
}

/// Rust `spawn_firecracker` — into the slot's namespace when there is one,
/// "so it can access tap0".
core::Expected<core::Unit, std::string> SpawnInto(FirecrackerInstance* instance,
                                                 const std::string& binary,
                                                 const core::Optional<std::string>& stdout_path,
                                                 const core::Optional<std::string>& stderr_path,
                                                 const std::string& netns) {
    const std::string out = stdout_path.has_value() ? *stdout_path : std::string();
    const std::string err = stderr_path.has_value() ? *stderr_path : std::string();
    if (netns.empty()) return instance->Spawn(binary, out, err);
    return instance->SpawnWithNetns(binary, out, err, netns);
}

}  // namespace

core::Expected<core::Unit, std::string>
StartFreshInstance(const FreshStartSpec& spec, std::shared_ptr<Connector> connector,
                   std::unique_ptr<FirecrackerInstance>* out,
                   FreshDriveResources* resources) {
    if (out == NULL) return core::make_unexpected(std::string("no output slot"));
    if (spec.drives.has_value() && resources == NULL) {
        return core::make_unexpected(
            std::string("fresh start with host drives needs a resource holder"));
    }
    if (spec.network.has_value() && resources == NULL) {
        return core::make_unexpected(
            std::string("fresh start with a network slot needs a resource holder"));
    }
    // 1. Work directory. The caller has already validated the configure plan,
    //    so nothing that fails below is knowable-up-front.
    //
    // One level under the base, as Rust's `create_firecracker_work_dir` does.
    // A `/firecracker/<sandbox_id>/` layer used to be inserted here, which
    // bought nothing -- `agentenv-fc-<random>` already makes the directory
    // unique per instance -- and cost 49 bytes of a 108-byte budget: the API
    // socket lives inside this directory and `sockaddr_un::sun_path` is 108
    // bytes including the NUL. With the extra layer the default configuration
    // produced a 107-byte path, one byte from Firecracker being unable to
    // bind, and no test could use a realistic temp directory.
    const core::Expected<std::string, std::string> work_dir =
        CreateFirecrackerWorkDir(core::Optional<std::string>(spec.chroot_base_dir));
    if (!work_dir.ok()) return core::make_unexpected(work_dir.error());
    // Destroyed last: the VMM is gone and its devices released before the
    // directory holding their symlinks and the API socket is removed.
    RemoveWorkDirOnFailure remove_work_dir = {work_dir.value(), true};

    // Declared before `instance` so it is destroyed *after* it: on an early
    // return the VMM is killed first and only then are its devices released.
    // Tearing a ublk device down under a live guest is what Rust's ordering
    // in `SandboxResources::release` exists to prevent.
    struct ReleaseOnFailure {
        FreshDriveResources* resources;
        bool armed;
        ~ReleaseOnFailure() {
            if (armed && resources != NULL) resources->Release();
        }
    } release_on_failure = {resources, spec.drives.has_value() || spec.network.has_value()};

    // 1b. Host drives (Rust `start_fresh`: link_tools_drive,
    //     attach_user_rootfs, prepare_extra_drives, and -- inside
    //     configure_microvm -- prepare/bind_volume_drive_slot). Firecracker
    //     only sees the relative names these create in the work directory.
    ConfigurePlan plan = spec.configure;
    if (spec.drives.has_value()) {
        const core::Expected<std::vector<DriveMount>, std::string> prepared =
            PrepareFreshDrives(work_dir.value(), *spec.drives, resources);
        if (!prepared.ok()) return core::make_unexpected(prepared.error());
        plan.extra_drive_attachments = prepared.value();

        const std::size_t physical = plan.physical_extra_drive_count;
        if (physical > plan.extra_drive_attachments.size()) {
            return core::make_unexpected(
                "fresh sandbox expects " + std::to_string(physical) +
                " physical extra drives, but only " +
                std::to_string(plan.extra_drive_attachments.size()) + " were prepared");
        }
        const std::vector<DriveMount> volumes(
            plan.extra_drive_attachments.begin() + static_cast<std::ptrdiff_t>(physical),
            plan.extra_drive_attachments.end());
        const core::Expected<core::Unit, std::string> slots =
            PrepareVolumeDriveSlots(work_dir.value(), plan.volume_drive_slots, volumes);
        if (!slots.ok()) return core::make_unexpected(slots.error());
    }

    // 1c. Network (Rust `start_fresh`, after the extra drives): allocate the
    //     slot, apply the egress policy, then compose the boot args with the
    //     slot's `ip=` and arp_notify ahead of the DAMON region. The interface
    //     and MMDS steps come with it; the VMM is spawned into the namespace
    //     below, which is the only way it can open `tap0`.
    std::string netns;
    if (spec.network.has_value()) {
        const FreshNetworkPlan& net = *spec.network;
        const core::Expected<network::Slot*, std::string> slot =
            AllocateNetworkSlot(net.manager, resources);
        if (!slot.ok()) return core::make_unexpected(slot.error());
        const core::Expected<core::Unit, std::string> policy =
            slot.value()->SetEgressPolicy(net.policy);
        if (!policy.ok()) {
            return core::make_unexpected("Failed to configure sandbox egress policy: " +
                                         policy.error());
        }
        std::vector<std::string> network_args;
        network_args.push_back(slot.value()->BuildIpBootArg());
        network_args.push_back(kGuestArpNotifyBootArg);
        plan.boot_args = ComposeFreshBootArgs(net.boot_args, net.extra_drives,
                                              plan.mem_size_mib, net.arch, network_args);
        plan.has_network_slot = true;
        plan.mmds_json = net.mmds_json;
        netns = slot.value()->NamespacePath();
    }

    // The logger path can only be known now; its emptiness was what decided
    // whether the step exists at all.
    std::vector<ConfigureStep> steps;
    {
        const core::Expected<std::vector<ConfigureStep>, std::string> built =
            BuildConfigureSequence(plan);
        if (!built.ok()) return core::make_unexpected(built.error());
        steps = built.value();
    }
    const FirecrackerLogPaths log_paths = ResolveFirecrackerLogPaths(
        spec.serial_output_base_dir, spec.stdout_path, spec.stderr_path, spec.capture_output,
        spec.sandbox_id, work_dir.value());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == ConfigureStep::Kind::Logger) {
            const core::Expected<core::Unit, std::string> dir = PrepareLoggerDir(log_paths);
            if (!dir.ok()) return core::make_unexpected(dir.error());
            steps[i].path = log_paths.logger_path;
        }
    }

    // 2. Spawn.
    const core::Optional<std::string>& stdout_path = log_paths.stdout_path;
    const core::Optional<std::string>& stderr_path = log_paths.stderr_path;

    if (!connector) connector = MakeUnixConnector();
    std::unique_ptr<FirecrackerInstance> instance(
        new FirecrackerInstance(work_dir.value(), connector));

    const core::Expected<core::Unit, std::string> spawned =
        SpawnInto(instance.get(), spec.firecracker_bin, stdout_path, stderr_path, netns);
    if (!spawned.ok()) {
        return core::make_unexpected(std::string("spawn: ") + spawned.error());
    }

    // Past the spawn, every early return must stop the VMM. `instance` is a
    // unique_ptr and ~FirecrackerInstance is this port's `Drop` (SIGKILL,
    // reap, unlink), so unwinding does it; only the success path moves the
    // instance out.

    // 3. Wait for the API socket. WaitForReady distinguishes "child exited"
    //    from "timed out" and quotes the captured stderr, which is the
    //    difference between a bad kernel path and a merely slow host.
    const core::Expected<core::Unit, std::string> ready =
        instance->WaitForReady(spec.socket_timeout_ms, spec.socket_poll_ms);
    if (!ready.ok()) return core::make_unexpected(ready.error());

    // 4. Apply the sequence.
    const core::Expected<core::Unit, std::string> configured =
        ApplyConfigureSequence(instance.get(), steps);
    if (!configured.ok()) return core::make_unexpected(configured.error());

    // 5. Start the microVM.
    const core::Expected<core::Unit, std::string> start = instance->Start();
    if (!start.ok()) {
        return core::make_unexpected(std::string("InstanceStart: ") + start.error());
    }
    // Only now is ownership handed over; every return above let the unique_ptr
    // unwind, which stops the VMM.
    *out = std::move(instance);
    release_on_failure.armed = false;
    remove_work_dir.armed = false;
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
StartResumeInstance(const ResumeStartSpec& spec, std::shared_ptr<Connector> connector,
                    std::unique_ptr<FirecrackerInstance>* out,
                    FreshDriveResources* resources) {
    if (out == NULL) return core::make_unexpected(std::string("no output slot"));
    if (spec.drives.has_value() && resources == NULL) {
        return core::make_unexpected(
            std::string("resume with host drives needs a resource holder"));
    }
    if (spec.network_manager != NULL && resources == NULL) {
        return core::make_unexpected(
            std::string("resume with a network slot needs a resource holder"));
    }
    if (spec.firecracker_pool != NULL && resources == NULL) {
        return core::make_unexpected(
            std::string("resume from the warm pool needs a resource holder"));
    }

    // The sequence is built first, exactly as the fresh path builds its
    // configure plan up front: every check inside is a property of the capture
    // itself, so finding one after a VMM is running means killing it for
    // something that was knowable before anything was created.
    std::vector<ResumeStep> steps;
    {
        const core::Expected<std::vector<ResumeStep>, std::string> built =
            BuildResumeSequence(spec);
        if (!built.ok()) return core::make_unexpected(built.error());
        steps = built.value();
    }

    // Destroyed last (declared first): the VMM goes, then its devices and
    // slot, then the directory -- Rust's `TempDir` drop for a failed launch.
    RemoveWorkDirOnFailure remove_work_dir = {std::string(), true};
    // Declared before `instance`, as on the fresh path, so the VMM is killed
    // before its devices are released.
    struct ReleaseOnFailure {
        FreshDriveResources* resources;
        bool armed;
        ~ReleaseOnFailure() {
            if (armed && resources != NULL) resources->Release();
        }
    } release_on_failure = {resources,
                            spec.drives.has_value() || spec.network_manager != NULL};
    std::unique_ptr<FirecrackerInstance> instance;
    network::Slot* slot = NULL;

    // Rust `adopt_warm_firecracker`, ahead of the drives: an adopted entry's
    // work dir becomes this sandbox's, so the drives are linked into the
    // directory its VMM already runs in.
    std::string work_dir;
    const bool adopted = AdoptWarmFirecracker(spec, resources, &instance, &slot, &work_dir);
    if (adopted) {
        release_on_failure.armed = true;
    } else {
        // Same layout as the fresh path -- see the note there on why the
        // per-sandbox layer is absent: the API socket inside this directory
        // has to fit in `sockaddr_un::sun_path`.
        const core::Expected<std::string, std::string> created =
            CreateFirecrackerWorkDir(core::Optional<std::string>(spec.chroot_base_dir));
        if (!created.ok()) return core::make_unexpected(created.error());
        work_dir = created.value();
    }
    remove_work_dir.path = work_dir;

    // Rust `start_resume`: link_tools_drive, attach_user_rootfs and
    // prepare_resume_drives, all before the spawn. The load reopens every
    // serialized drive path, so each has to exist by then. The new-volume
    // binds it returns are the same ones `BuildResumeSequence` derived from
    // the same indices; they are bound and patched after the load.
    if (spec.drives.has_value()) {
        std::vector<std::size_t> new_indices;
        for (std::size_t i = 0; i < spec.initial_guest_drive_mounts.size(); ++i) {
            new_indices.push_back(spec.initial_guest_drive_mounts[i].first);
        }
        const core::Expected<std::vector<VolumeSlotBind>, std::string> prepared =
            PrepareResumeDrives(work_dir, *spec.drives, spec.snapshot_config.common,
                                new_indices, resources);
        if (!prepared.ok()) return core::make_unexpected(prepared.error());
    }

    const CommonConfig& common = spec.snapshot_config.common;
    const FirecrackerLogPaths log_paths = ResolveFirecrackerLogPaths(
        common.serial_output_base_dir, common.stdout_path, common.stderr_path,
        spec.capture_output, spec.sandbox_id, work_dir);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == ResumeStep::Kind::Logger) {
            const core::Expected<core::Unit, std::string> dir = PrepareLoggerDir(log_paths);
            if (!dir.ok()) return core::make_unexpected(dir.error());
            steps[i].path = log_paths.logger_path;
        }
    }

    // Rust "Network + Firecracker spawn (a warm Firecracker already has
    // both)": the slot after the drives, the VMM into its namespace so the
    // load's `eth0 -> tap0` override opens a tap that exists.
    const bool needs_socket_wait = !adopted;
    if (needs_socket_wait) {
        if (spec.network_manager != NULL) {
            const core::Expected<network::Slot*, std::string> allocated =
                AllocateNetworkSlot(spec.network_manager, resources);
            if (!allocated.ok()) return core::make_unexpected(allocated.error());
            slot = allocated.value();
        }
        if (!connector) connector = MakeUnixConnector();
        instance.reset(new FirecrackerInstance(work_dir, connector));
        const core::Expected<core::Unit, std::string> spawned =
            SpawnInto(instance.get(), spec.firecracker_bin, log_paths.stdout_path,
                      log_paths.stderr_path,
                      slot != NULL ? slot->NamespacePath() : std::string());
        if (!spawned.ok()) {
            return core::make_unexpected(std::string("spawn: ") + spawned.error());
        }
    }

    // Rust applies the capture's policy right after the spawn: a pooled slot
    // keeps its namespace across tenants, so a previous tenant's rules are
    // cleared here even when this capture has none.
    if (slot != NULL) {
        const core::Expected<core::Unit, std::string> policy =
            slot->SetEgressPolicy(spec.snapshot_config.common.network_policy);
        if (!policy.ok()) {
            return core::make_unexpected(
                "Failed to configure sandbox egress policy for resume: " + policy.error());
        }
    }

    // Past the spawn every early return must stop the VMM; the unique_ptr's
    // destructor is this port's `Drop` and does it on unwind. A warm VMM is
    // already answering on its socket.
    if (needs_socket_wait) {
        const core::Expected<core::Unit, std::string> ready =
            instance->WaitForReady(spec.socket_timeout_ms, spec.socket_poll_ms);
        if (!ready.ok()) return core::make_unexpected(ready.error());
    }

    // No `InstanceStart` on this path: the VM comes into existence already
    // running from the loaded state, and the sequence's own final step is the
    // resume. Issuing a start as well would be rejected -- and if it were not,
    // it would boot a second machine over the restored one.
    const core::Expected<core::Unit, std::string> applied =
        ApplyResumeSequence(instance.get(), steps);
    if (!applied.ok()) return core::make_unexpected(applied.error());

    // The resources now belong to the running VM; disarming is what keeps the
    // guards from releasing its devices, slot and directory out from under it.
    *out = std::move(instance);
    release_on_failure.armed = false;
    remove_work_dir.armed = false;
    return core::Unit{};
}

class FirecrackerSandboxBackend::Impl {
 public:
    Impl(std::unique_ptr<FirecrackerInstance> i, SnapshotConfig cfg)
        : instance(std::move(i)), snapshot_config(std::move(cfg)), startable(false) {}

    Impl(FreshStartSpec s, SnapshotConfig cfg, std::shared_ptr<Connector> c)
        : snapshot_config(std::move(cfg)), spec(std::move(s)), connector(std::move(c)),
          startable(true) {}

    /// Resume form. `spec` is filled from the resume spec's shared fields so
    /// `WaitForReady` and `Snapshot` -- which read the envd timeouts and the
    /// work dir -- behave the same on both paths.
    Impl(ResumeStartSpec s, std::shared_ptr<Connector> c)
        : snapshot_config(s.snapshot_config), resume_spec(std::move(s)),
          connector(std::move(c)), startable(true) {
        launch.kind = LaunchMode::Kind::Resume;
        spec.firecracker_bin = resume_spec.firecracker_bin;
        spec.chroot_base_dir = resume_spec.chroot_base_dir;
        spec.sandbox_id = resume_spec.sandbox_id;
        spec.firecracker_work_dir = resume_spec.firecracker_work_dir;
        spec.capture_output = resume_spec.capture_output;
        spec.socket_timeout_ms = resume_spec.socket_timeout_ms;
        spec.socket_poll_ms = resume_spec.socket_poll_ms;
        spec.envd_timeout_ms = resume_spec.envd_timeout_ms;
        spec.envd_poll_ms = resume_spec.envd_poll_ms;
        spec.control_plane_port = resume_spec.snapshot_config.common.control_plane_port;
        spec.envd_access_token = resume_spec.snapshot_config.common.envd_access_token;
    }

    /// Rust `Drop`: the VMM goes first, then its devices. Member order would
    /// do the opposite (`drive_resources` is declared after `instance`), and a
    /// device deleted under a running guest fails every in-flight I/O.
    ~Impl() {
        instance.reset();
        (void)drive_resources.Release();
        // Rust drops `work_dir: TempDir` with the sandbox, after the VMM and
        // its resources are gone.
        if (!owned_work_dir.empty()) (void)core::fs::RemoveDirAll(owned_work_dir);
    }

    std::unique_ptr<FirecrackerInstance> instance;
    /// Rust `work_dir: TempDir` — the per-instance directory a start created
    /// (or adopted from the warm pool). Empty for a handed-in instance, whose
    /// directory belongs to the caller.
    std::string owned_work_dir;
    /// Rust `SandboxResources` — the ublk devices behind this VM's drives.
    FreshDriveResources drive_resources;
    SnapshotConfig snapshot_config;
    FreshStartSpec spec;
    /// Only meaningful when `launch.kind == Resume`.
    ResumeStartSpec resume_spec;
    std::shared_ptr<Connector> connector;
    /// False for a backend handed an already-booted instance: there is nothing
    /// left to start, and a start would spawn a second VMM over the first.
    bool startable;
    LaunchMode launch;
    EnvdInstance::HealthProbe envd_probe;
    /// Rust `envd_instance: Option<EnvdInstance>`.
    ///
    /// Held rather than rebuilt per call, which is what makes `Envd()`
    /// meaningful: `EnvdInstance` shares its liveness flag between copies, so
    /// invalidating *this* one on teardown invalidates every handle the
    /// orchestrator took from it. Rebuilding a fresh instance on each call
    /// would hand out handles that can never be invalidated, and one taken
    /// before teardown would keep talking to whatever VM next holds that IP.
    std::shared_ptr<EnvdInstance> envd_instance;
    /// Test override for the guest-memory layer write.
    MemoryLayerWriter memory_layer_writer;

    /// Rust `self.launch.common().extra_drives`.
    ///
    /// The three construction forms keep their drives in different places, so
    /// a single accessor is what lets the volume predicates below be written
    /// once:
    ///   * resume   -> the snapshot config being restored;
    ///   * fresh    -> the host-side drive plan, which is what actually gets
    ///                 attached;
    ///   * handed-in instance -> no plan ran here, so the capture descriptor
    ///                 it was constructed with is the only record of them.
    ///
    /// The fallback matters: a backend built from an already-booted instance
    /// is what `Pause` hands back, and reporting "no drives" for it would say
    /// a sandbox holding writable volumes has none to flush.
    const std::vector<ExtraDrive>& ExtraDrives() const {
        if (launch.kind == LaunchMode::Kind::Resume) {
            return resume_spec.snapshot_config.common.extra_drives;
        }
        if (spec.drives.has_value()) return spec.drives->extra_drives;
        return snapshot_config.common.extra_drives;
    }

    /// Rust `has_writable_persistent_volumes` — a drive that is writable
    /// *and* has a snapshot output dir.
    ///
    /// Both halves are required: a read-only volume has nothing to flush or
    /// seal, and a writable drive with no output dir is an ordinary attached
    /// drive whose contents live in the snapshot rather than in a volume.
    bool HasWritablePersistentVolumes() const {
        const std::vector<ExtraDrive>& drives = ExtraDrives();
        for (std::size_t i = 0; i < drives.size(); ++i) {
            if (!drives[i].read_only && drives[i].snapshot_output_dir.has_value()) return true;
        }
        return false;
    }

    /// Rust `live_snapshot_root` — resolved once, then reused.
    ///
    /// The caching is not an optimisation: Rust holds the root in a guard for
    /// the sandbox's lifetime, so every capture of one sandbox lands under the
    /// same directory. Re-deriving it per call would be equivalent only as
    /// long as nothing in the derivation ever changes.
    std::string live_snapshot_root;

    core::Expected<std::string, std::string> LiveSnapshotRoot() {
        if (!live_snapshot_root.empty()) return live_snapshot_root;
        const std::string root = ManagedSnapshotBase(spec.firecracker_work_dir) + "/" +
                                 spec.sandbox_id.ToString();
        // Rust `PersistentSnapshotRootGuard::prepare`.
        const core::Expected<core::Unit, std::string> made = MakeDirs(root);
        if (!made.ok()) return core::make_unexpected(made.error());
        live_snapshot_root = root;
        return live_snapshot_root;
    }

    /// Rust `recover_capture_failure`.
    ///
    /// A terminal error is returned as-is: the runtime is already mutated, so
    /// resuming would not make the sandbox trustworthy again. Otherwise the VM
    /// is put back into a running state *before* the error is reported,
    /// because `Recoverable` is a promise to the caller that the sandbox is
    /// still runnable. If that resume fails, the promise cannot be kept and
    /// the error is upgraded to `Terminal` — carrying both messages, since the
    /// original fault is what an operator needs and the resume failure is what
    /// makes it unrecoverable.
    SandboxCaptureError RecoverCaptureFailure(const char* operation,
                                              const SandboxCaptureError& error) {
        if (error.IsTerminal()) return error;
        const core::Expected<core::Unit, std::string> resumed = instance->Resume();
        if (!resumed.ok()) {
            return SandboxCaptureError::Terminal(
                std::string(operation) + " failed and sandbox could not be resumed: " +
                error.message + "; resume error: " + resumed.error());
        }
        return error;
    }
};

FirecrackerSandboxBackend::FirecrackerSandboxBackend(
    std::unique_ptr<FirecrackerInstance> instance, SnapshotConfig snapshot_config)
    : impl_(new Impl(std::move(instance), std::move(snapshot_config))) {}

FirecrackerSandboxBackend::FirecrackerSandboxBackend(
    FreshStartSpec spec, SnapshotConfig snapshot_config,
    std::shared_ptr<Connector> connector)
    : impl_(new Impl(std::move(spec), std::move(snapshot_config), std::move(connector))) {}

FirecrackerSandboxBackend::~FirecrackerSandboxBackend() = default;

FirecrackerSandboxBackend* FirecrackerSandboxBackend::NewResuming(
    ResumeStartSpec spec, std::shared_ptr<Connector> connector) {
    // A private constructor would be cleaner, but `Impl` is incomplete in the
    // header; a static factory keeps the resume form distinguishable from the
    // fresh one at the call site, which is the part that matters -- the two
    // send the VMM different things.
    FirecrackerSandboxBackend* backend = new FirecrackerSandboxBackend(
        FreshStartSpec(), SnapshotConfig(), std::shared_ptr<Connector>());
    backend->impl_.reset(new Impl(std::move(spec), std::move(connector)));
    return backend;
}

void FirecrackerSandboxBackend::SetEnvdProbeForTesting(
    const EnvdInstance::HealthProbe& probe) {
    impl_->envd_probe = probe;
}

SandboxCaptureResult<std::shared_ptr<PausedSandboxState> >
FirecrackerSandboxBackend::Pause(const core::Optional<std::string>& artifact_root) {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("pause: no live microVM"));
    }
    // Writing capture artifacts into a caller-managed directory is Rust's
    // `pause_to_dir`, which also flushes writable volumes through envd. Neither
    // is ported, so an artifact root is refused rather than ignored: honouring
    // the pause but dropping the artifacts would hand back a paused state whose
    // files were never written.
    if (artifact_root.has_value()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "pause to a caller-managed artifact root is not available in this build"));
    }

    const core::Expected<core::Unit, std::string> paused = impl_->instance->Pause();
    if (!paused.ok()) {
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("pause", ClassifyCaptureError(paused.error())));
    }
    return std::shared_ptr<PausedSandboxState>(
        new FirecrackerPausedState(impl_->snapshot_config));
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Resume() {
    if (!impl_->instance) return core::make_unexpected(core::err("resume: no live microVM"));
    // Forwarded unconditionally. Firecracker accepts a resume on a running VM,
    // which is where the trait's idempotence comes from; a local paused flag
    // could only drift from the VMM's actual state.
    const core::Expected<core::Unit, std::string> r = impl_->instance->Resume();
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Stop() {
    // Idempotent, as the trait requires: with no instance there is nothing to
    // stop and that is a success, not an error. `Stop` on an already-dead
    // child reaps it and removes the socket, so a second call is also fine.
    if (!impl_->instance) return core::Unit{};
    // Invalidated *before* the VMM goes, and unconditionally: the flag is
    // shared with every handle already handed out, and the window between
    // "VMM gone" and "handles refuse" is exactly when a sampling sweep would
    // connect to an IP the next sandbox has been given. Done even if the stop
    // below fails, because a half-stopped sandbox is no more safe to talk to.
    if (impl_->envd_instance) impl_->envd_instance->Invalidate();
    const core::Expected<core::Unit, std::string> r =
        impl_->instance->Stop(kPoolFirecrackerStopTimeoutMs);
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    // Rust `stop`: devices are released only once the VMM is gone. A failed
    // stop keeps them, so a retry still has something to release. Only the
    // network release can fail the stop, and only after every device went.
    const core::Expected<core::Unit, std::string> released = impl_->drive_resources.Release();
    if (!released.ok()) return core::make_unexpected(core::err(released.error()));
    return core::Unit{};
}

SandboxRuntimeInfo FirecrackerSandboxBackend::RuntimeInfoOf() const {
    SandboxRuntimeInfo info;
    // `rootfs_virtual_size` is cached by Rust's `start()` from the live block
    // device and is required before a snapshot; this port never records it, so
    // it stays absent rather than being guessed. `RuntimeArtifacts` comes from
    // the capture descriptor, which is the same source Rust uses.
    info.runtime_artifacts = FirecrackerPausedState(impl_->snapshot_config).RuntimeArtifacts();
    return info;
}

SandboxCaptureResult<PauseToDirResult>
FirecrackerSandboxBackend::PauseToDir(const PauseToDirSpec& spec) {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("pause_to_dir: no live microVM"));
    }

    // 1. Flush writable volumes -- BEFORE the pause. The flush runs inside the
    //    guest through envd, which cannot be reached once the VM is paused.
    //    Doing this after would capture volumes with dirty page cache, so the
    //    resumed sandbox would see a stale filesystem rather than an old one.
    if (spec.has_writable_persistent_volumes) {
        if (!spec.flush_volumes) {
            return core::make_unexpected(SandboxCaptureError::Recoverable(
                "cannot flush writable volumes because envd is not running"));
        }
        const core::Expected<core::Unit, std::string> flushed = spec.flush_volumes();
        if (!flushed.ok()) {
            // Nothing has been paused or created yet, so this is recoverable
            // without any recovery: the sandbox is still running.
            return core::make_unexpected(SandboxCaptureError::Recoverable(flushed.error()));
        }
    }

    // 2. Pause.
    const core::Expected<core::Unit, std::string> paused = impl_->instance->Pause();
    if (!paused.ok()) {
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("pause", ClassifyCaptureError(paused.error())));
    }

    // From here the VM is paused, so every failure must run the recovery
    // resume *and* remove the partial directory. A half-written snapshot
    // directory cannot be told from a complete one by name, and the resume path
    // would later load a vm_state.bin whose memory layer was never finished.
    PauseToDirResult result;
    const core::Expected<core::Unit, std::string> captured = [&]()
        -> core::Expected<core::Unit, std::string> {
        const core::Expected<core::Unit, std::string> made = MakeDirs(spec.snapshot_dir);
        if (!made.ok()) return made;

        // 3. Read the pid BEFORE the state snapshot. Guest memory is pulled
        //    from the live VMM with process_vm_readv, so the capture needs a
        //    pid that is still valid; reading it after would race a VMM that
        //    died during the snapshot and blame the wrong step.
        const core::Expected<int64_t, std::string> pid = impl_->instance->Pid();
        if (!pid.ok()) return core::make_unexpected(pid.error());
        if (spec.on_pid_read) spec.on_pid_read(pid.value());

        result.vm_state_path = spec.snapshot_dir + "/" + kVmStateFileName;
        const core::Expected<core::Unit, std::string> snap =
            impl_->instance->CreateStateOnlySnapshot(result.vm_state_path);
        if (!snap.ok()) return core::make_unexpected(snap.error());

        // 4. vm_state.bin now describes this paused VM. Dirty ranges must be
        //    read after it, so the two describe the same instant.
        const core::Expected<DirtyMemoryRanges, std::string> ranges =
            impl_->instance->GetDirtyMemoryRanges();
        if (!ranges.ok()) return core::make_unexpected(ranges.error());

        uint64_t memory_size = 0;
        const core::Expected<std::vector<storage::overlaybd::lsmt::SegmentMapping>,
                             std::string>
            mappings = DirtyRangesToSegmentMappings(ranges.value(), &memory_size);
        if (!mappings.ok()) return core::make_unexpected(mappings.error());

        const std::string mem_dir = spec.snapshot_dir + "/mem_overlaybd";
        const core::Expected<core::Unit, std::string> mem_made = MakeDirs(mem_dir);
        if (!mem_made.ok()) return mem_made;

        result.memory_layer_path = mem_dir + "/overlaybd.commit";
        result.memory_size = memory_size;
        if (spec.write_memory_layer) {
            return spec.write_memory_layer(result.memory_layer_path, mappings.value(),
                                           memory_size, pid.value());
        }
        return PublishMemoryOverlaybdLayer(result.memory_layer_path, mappings.value(),
                                           memory_size, pid.value());
    }();

    if (!captured.ok()) {
        CleanupFailedSnapshotDir(spec.snapshot_dir);
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("snapshot capture",
                                         ClassifyCaptureError(captured.error())));
    }
    return result;
}

SandboxCaptureResult<std::string> FirecrackerSandboxBackend::Snapshot() {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("snapshot: no live microVM"));
    }

    const core::Expected<std::string, std::string> root = impl_->LiveSnapshotRoot();
    if (!root.ok()) {
        // Nothing has been paused yet, so the sandbox is untouched and this
        // needs no recovery.
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            std::string("snapshot: ") + root.error()));
    }

    PauseToDirSpec spec;
    // A fresh v7 id per capture: time-ordered, so a directory listing is in
    // capture order, and collision-free, so two snapshots of one sandbox never
    // write into each other.
    spec.snapshot_dir = root.value() + "/" + core::Uuid::GenV7().ToString();
    // Rust reads `has_writable_persistent_volumes()` off the sandbox's own
    // drives, and so does this now. Hard-coding false was safe only while no
    // drive could reach this backend; with volumes wired up it would skip the
    // pre-pause flush for a sandbox that has writable volumes, capturing them
    // with dirty guest page cache. PauseToDir refuses that rather than
    // flushing nothing, because this build has no envd client to flush
    // through -- a stale filesystem is worse than a refused snapshot.
    spec.has_writable_persistent_volumes = impl_->HasWritablePersistentVolumes();
    spec.write_memory_layer = impl_->memory_layer_writer;

    const SandboxCaptureResult<PauseToDirResult> captured = PauseToDir(spec);
    if (!captured.ok()) {
        // PauseToDir has already run the recovery resume and removed the
        // partial directory; re-running either here would resume a sandbox
        // that a terminal error says must not be trusted.
        return core::make_unexpected(captured.error());
    }

    // Rust `map_err(SandboxCaptureError::terminal)`: unconditionally terminal,
    // and not routed through RecoverCaptureFailure. The capture is on disk and
    // intact, so there is nothing to retry -- what failed is putting the
    // source sandbox back, and a caller that kept using it would be driving a
    // VM frozen at the moment of the snapshot.
    const core::Expected<core::Unit, std::string> resumed = impl_->instance->Resume();
    if (!resumed.ok()) {
        return core::make_unexpected(SandboxCaptureError::Terminal(
            "snapshot captured but sandbox could not be resumed: " + resumed.error()));
    }
    return spec.snapshot_dir;
}

void FirecrackerSandboxBackend::SetMemoryLayerWriterForTesting(
    const MemoryLayerWriter& writer) {
    impl_->memory_layer_writer = writer;
}

// ---- not available in this build ------------------------------------------

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::StartNowait() {
    if (!impl_->startable) {
        return core::make_unexpected(
            core::err("start_nowait: this backend was handed an already-booted microVM"));
    }
    if (impl_->instance) {
        // A second spawn would leak the first VMM and put two processes on one
        // socket path, where the second's requests reach whichever bound it.
        return core::make_unexpected(core::err("start_nowait: sandbox is already started"));
    }
    // Rust `match &self.launch { Fresh => start_fresh, Resume => start_resume }`.
    // The two send the VMM different things -- a boot source plus drive
    // attachments against `PUT /snapshot/load` -- so the dispatch is the whole
    // reason `LaunchMode` exists rather than being collapsed to a flag.
    std::unique_ptr<FirecrackerInstance> started;
    const core::Expected<core::Unit, std::string> r =
        impl_->launch.kind == LaunchMode::Kind::Resume
            ? StartResumeInstance(impl_->resume_spec, impl_->connector, &started,
                                  &impl_->drive_resources)
            : StartFreshInstance(impl_->spec, impl_->connector, &started,
                                 &impl_->drive_resources);
    if (!r.ok()) return core::make_unexpected(core::err(r.error()));
    impl_->instance = std::move(started);
    impl_->owned_work_dir = impl_->instance->WorkDir();
    return core::Unit{};
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::WaitForReady() {
    if (!impl_->instance) {
        return core::make_unexpected(core::err("wait_for_ready: sandbox is not started"));
    }
    // Rust `init_envd_instance(interaction_ip, common)` ran at start: envd is
    // reached at `http://<host_interaction_ip>:<control_plane_port>`. Without
    // a slot there is no address, which is Rust's "not initialized" -- and
    // claiming readiness never observed would hand the orchestrator a sandbox
    // that accepts work and answers nothing.
    if (impl_->envd_probe) {
        // Test override for the probe; the readiness loop is the real one.
        EnvdInstance envd(std::string("http://127.0.0.1"));
        const core::Expected<core::Unit, std::string> r = envd.WaitForReady(
            impl_->spec.envd_timeout_ms, impl_->spec.envd_poll_ms, impl_->envd_probe);
        if (!r.ok()) return core::make_unexpected(core::err(r.error()));
        return core::Unit{};
    }
    const core::Optional<std::string> interaction_ip = HostInteractionIp();
    if (!interaction_ip.has_value()) {
        return core::make_unexpected(core::err("wait_for_ready: envd instance not initialized"));
    }
    // Rust `init_envd_instance` stores the instance on the sandbox; this is
    // the point at which the address is known, so it is where the handle
    // `Envd()` hands out comes into being.
    impl_->envd_instance.reset(
        new EnvdInstance("http://" + *interaction_ip + ":" +
                             std::to_string(impl_->spec.control_plane_port),
                         impl_->spec.envd_access_token));
    const core::Expected<core::Unit, std::string> r = impl_->envd_instance->WaitForReady(
        impl_->spec.envd_timeout_ms, impl_->spec.envd_poll_ms);
    if (!r.ok()) {
        // Readiness was never observed, so the instance must not be handed
        // out: a caller would send guest I/O to an address that answered
        // nothing. Dropped rather than kept-and-invalidated because this one
        // was never published to anybody.
        impl_->envd_instance.reset();
        return core::make_unexpected(core::err(r.error()));
    }
    return core::Unit{};
}

std::shared_ptr<EnvdApi> FirecrackerSandboxBackend::Envd() const {
    // Nothing until `WaitForReady` has observed a healthy guest. Returning a
    // handle earlier would be a handle to an agent that is not answering yet,
    // and the caller has no way to tell that from one that has stopped.
    return impl_->envd_instance;
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::Start() {
    const core::Expected<core::Unit, core::AnyError> started = StartNowait();
    if (!started.ok()) return started;
    return WaitForReady();
}
// Rust `snapshot_volumes` / `freeze_and_snapshot_volumes` / `thaw_volumes`.
//
// Each of these begins by selecting the *writable persistent volumes*, and
// every step that follows is driven by that selection. When it is empty they
// are no-ops upstream, so reporting "not available" for a sandbox that has no
// such volume refused a capture Rust completes — which is every sandbox in
// this build that attaches no volume, i.e. the common case.
//
// The sealing half is what is genuinely missing, and it stays an error:
// `snapshot_persistent_volume_drives` restacks each volume's overlaybd device
// into a new commit layer (`restack_snapshot_overlaybd_device`), and this port
// has the LSMT index types but no layer writer. Reporting success without it
// would publish a volume whose snapshot layer was never written, and the
// damage would surface on another node, at a later resume, as a volume that
// silently lost every write since its last good capture.
SandboxCaptureResult<core::Unit> FirecrackerSandboxBackend::SnapshotVolumes() {
    // Rust: `if !self.has_writable_persistent_volumes() { return Ok(()) }`.
    if (!impl_->HasWritablePersistentVolumes()) return core::Unit();
    return core::make_unexpected(SandboxCaptureError::Recoverable(
        "snapshot_volumes: sealing a writable persistent volume needs the overlaybd layer "
        "writer and an envd client to flush through, neither of which is available in this "
        "build"));
}

SandboxCaptureResult<core::Unit> FirecrackerSandboxBackend::FreezeAndSnapshotVolumes() {
    // Rust freezes each writable persistent volume's mount, then seals them.
    // With no such mount the loop body never runs and the seal is a no-op, so
    // the whole operation succeeds without reaching the guest.
    //
    // The commands themselves are ported (`sandbox/guest_volumes.h`:
    // `SetVolumeFreeze`, `SyncWritableVolumeFilesystems`), so what is missing
    // is narrower than it was: an `Executor` that dispatches into the guest.
    // That gap is not bridgeable with the host-local executor — running
    // `fsfreeze` on the host would freeze a *host* filesystem by the same
    // path, which is why no fallback is attempted here.
    if (!impl_->HasWritablePersistentVolumes()) return core::Unit();
    return core::make_unexpected(SandboxCaptureError::Recoverable(
        "freeze_and_snapshot_volumes: freezing a volume filesystem needs a guest-dispatching "
        "executor to run the fsfreeze ioctl inside the VM, which is not available in this "
        "build"));
}

core::Expected<core::Unit, core::AnyError> FirecrackerSandboxBackend::ThawVolumes() {
    // Rust drains `frozen_volume_mounts`, thawing each. This backend never
    // freezes anything -- `FreezeAndSnapshotVolumes` fails before it reaches
    // the guest and records no mount -- so there is never a thaw obligation
    // outstanding and the drain is empty.
    //
    // Reporting an error here instead would be actively harmful: thaw is the
    // cleanup half of a failed capture, and a caller that is told the thaw
    // failed must treat the sandbox as unusable. That would condemn a sandbox
    // over cleanup work that was never needed.
    return core::Unit{};
}
// Rust `fork`.
//
// The source is paused to get a consistent capture, resumed immediately, and
// each child is then built from that one capture. Two orderings here are
// load-bearing:
//
//  * The source is resumed **before** any child is built. Rust does the same,
//    and it is what keeps a fork from holding the parent frozen for as long as
//    the children take to boot. A build failure after this point is a child's
//    problem, not the parent's.
//  * A pause failure goes through `RecoverCaptureFailure`, which resumes the
//    VM before reporting anything recoverable. Reporting a recoverable error
//    while the VM is still paused would tell the orchestrator the sandbox is
//    fine when it is frozen.
//
// A failed *resume* is terminal: the capture happened, the VM is paused, and
// nothing here can get it running again -- so the source must not be treated
// as still serving.
SandboxCaptureResult<std::vector<SandboxForkResult> >
FirecrackerSandboxBackend::Fork(const std::vector<SandboxForkSpec>& specs) {
    if (!impl_->instance) {
        return core::make_unexpected(
            SandboxCaptureError::Terminal("fork: no live microVM"));
    }

    const core::Expected<core::Unit, std::string> paused = impl_->instance->Pause();
    if (!paused.ok()) {
        return core::make_unexpected(
            impl_->RecoverCaptureFailure("fork snapshot", ClassifyCaptureError(paused.error())));
    }
    const SnapshotConfig captured = impl_->snapshot_config;

    const core::Expected<core::Unit, std::string> resumed = impl_->instance->Resume();
    if (!resumed.ok()) {
        return core::make_unexpected(SandboxCaptureError::Terminal(
            std::string("fork: failed to resume source after capture: ") + resumed.error()));
    }

    std::vector<SandboxForkResult> results;
    results.reserve(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const SandboxForkSpec& child = specs[i];

        // A replacement may only target a *volume* slot. The physical drives
        // occupy the slots before `physical_extra_drive_count`, and the guest
        // names devices positionally -- so swapping a volume into a physical
        // slot would mount it as the rootfs or a layer, not as the volume.
        bool targets_physical = false;
        const std::size_t physical_count = captured.common.physical_extra_drive_count;
        for (std::size_t k = 0; k < child.replace_drive_ids.size() && !targets_physical; ++k) {
            const std::string& target = child.replace_drive_ids[k].first;
            const std::size_t limit = physical_count < captured.common.extra_drives.size()
                                          ? physical_count
                                          : captured.common.extra_drives.size();
            for (std::size_t d = 0; d < limit; ++d) {
                if (captured.common.extra_drives[d].drive_id == target) {
                    targets_physical = true;
                    break;
                }
            }
        }
        if (targets_physical) {
            SandboxForkResult refused = core::make_unexpected(core::err(
                "fork replacement drives must refer to launch-time volume drives"));
            results.push_back(std::move(refused));
            continue;
        }

        core::Expected<SnapshotConfig, std::string> child_config =
            SnapshotConfigForFork(captured, child);
        if (!child_config.ok()) {
            SandboxForkResult failed = core::make_unexpected(core::err(child_config.error()));
            results.push_back(std::move(failed));
            continue;
        }

        // Rust's `from_snapshot_config_with_override`: the child's identity and
        // its own envd token replace the captured ones. Skipping this would
        // leave the child announcing the source's id through MMDS and holding
        // the source's live credential.
        SnapshotConfig prepared = child_config.value();
        const core::Expected<core::Unit, std::string> ready =
            PrepareSnapshotConfigForResume(&prepared, child.sandbox_id,
                                           child.envd_access_token);
        if (!ready.ok()) {
            SandboxForkResult failed = core::make_unexpected(
                core::err(std::string("build forked sandbox: ") + ready.error()));
            results.push_back(std::move(failed));
            continue;
        }

        // The launch parameters come from this sandbox's own spec, so a child
        // uses the same binary, chroot base and timeouts as its parent.
        ResumeStartSpec spec;
        spec.snapshot_config = prepared;
        spec.firecracker_bin = impl_->spec.firecracker_bin;
        spec.chroot_base_dir = impl_->spec.chroot_base_dir;
        spec.sandbox_id = child.sandbox_id;
        spec.firecracker_work_dir = impl_->spec.firecracker_work_dir;
        spec.capture_output = impl_->spec.capture_output;
        spec.socket_timeout_ms = impl_->spec.socket_timeout_ms;
        spec.socket_poll_ms = impl_->spec.socket_poll_ms;
        spec.envd_timeout_ms = impl_->spec.envd_timeout_ms;
        spec.envd_poll_ms = impl_->spec.envd_poll_ms;
        // Inherited from the resume spec when this sandbox was itself resumed.
        // Empty on a sandbox that booted fresh, and the resume path reports
        // that as its own error rather than guessing a path.
        spec.mem_device_path = impl_->resume_spec.mem_device_path;
        // Rust forks through `start_resume`, which materializes the child's
        // drives from the same node config the parent used.
        if (impl_->resume_spec.drives.has_value()) {
            spec.drives = impl_->resume_spec.drives;
        } else if (impl_->spec.drives.has_value()) {
            ResumeDrivePlan drives;
            drives.device_manager = impl_->spec.drives->device_manager;
            drives.tools_drive_path = impl_->spec.drives->tools_drive_path;
            // A fork must land on the parent's tools release, not the node's
            // current one: the child resumes the parent's capture.
            drives.tools_drive_version = impl_->spec.drives->tools_drive_version;
            drives.tools_drive_resolver = impl_->spec.drives->tools_drive_resolver;
            drives.overlaybd_global_config = impl_->spec.drives->overlaybd_global_config;
            drives.runtime_upper_mode = impl_->spec.drives->runtime_upper_mode;
            drives.extra_drive_allow_shrink = impl_->spec.drives->extra_drive_allow_shrink;
            spec.drives = drives;
        }
        // Each child gets its own slot from the same manager; sharing the
        // parent's would put two guests behind one tap.
        spec.network_manager = impl_->resume_spec.network_manager != NULL
                                   ? impl_->resume_spec.network_manager
                               : impl_->spec.network.has_value()
                                   ? impl_->spec.network->manager
                                   : NULL;
        // Rust forks through `start_resume`, whose `adopt_warm_firecracker`
        // reads the same global pool for every child.
        spec.firecracker_pool = impl_->resume_spec.firecracker_pool != NULL
                                    ? impl_->resume_spec.firecracker_pool
                                    : impl_->spec.firecracker_pool;

        std::unique_ptr<SandboxBackend> backend(
            FirecrackerSandboxBackend::NewResuming(spec, impl_->connector));

        // Started here, and stopped on failure before being dropped: a child
        // whose start failed may still have left a VMM process behind, and
        // dropping the backend would leak it with nothing holding its id.
        const core::Expected<core::Unit, core::AnyError> started = backend->Start();
        if (!started.ok()) {
            const core::Expected<core::Unit, core::AnyError> stopped = backend->Stop();
            if (!stopped.ok()) {
                AGENTENV_WARN("failed to stop fork child after start failure: "
                              << stopped.error().chain());
            }
            SandboxForkResult failed = core::make_unexpected(
                core::err(std::string("start forked sandbox: ") + started.error().chain()));
            results.push_back(std::move(failed));
            continue;
        }

        results.push_back(SandboxForkResult(std::move(backend)));
    }

    // The call succeeds even when every child failed: a per-child failure is
    // reported inside the vector, and failing the whole call would tell the
    // orchestrator the *source* broke.
    return SandboxCaptureResult<std::vector<SandboxForkResult> >(std::move(results));
}
core::Optional<std::string> FirecrackerSandboxBackend::HostInteractionIp() const {
    // Rust: `network_slot.map(|slot| slot.host_interaction_ip)` -- `None`
    // before start and after stop, when there is no slot.
    const network::Slot* slot = impl_->drive_resources.network_slot();
    if (slot == NULL) return core::nullopt;
    return network::Ipv4ToString(slot->HostInteractionIp());
}
RuntimeArtifactSet FirecrackerSandboxBackend::StartupArtifacts() const {
    return FirecrackerPausedState(impl_->snapshot_config).RuntimeArtifacts();
}
core::Expected<core::Unit, core::AnyError>
FirecrackerSandboxBackend::UpdateNetworkPolicy(const core::Optional<std::string>& policy_json) {
    core::Optional<network::SandboxNetworkPolicy> policy;
    if (policy_json.has_value()) {
        const core::Expected<core::Json, core::AnyError> doc = core::Json::Parse(*policy_json);
        if (!doc.ok()) {
            return core::make_unexpected(
                core::err("parse sandbox network policy: " + doc.error().chain()));
        }
        const core::Expected<network::SandboxNetworkPolicy, std::string> parsed =
            network::SandboxNetworkPolicy::FromJson(doc.value());
        if (!parsed.ok()) {
            return core::make_unexpected(
                core::err("parse sandbox network policy: " + parsed.error()));
        }
        policy = parsed.value();
    }

    // Rust `update_network_policy`: applied to a live slot; with no slot only
    // clearing is accepted, since a policy with nowhere to go would be a
    // restriction the caller believes is enforced.
    network::Slot* slot = impl_->drive_resources.network_slot();
    if (slot != NULL) {
        const core::Expected<core::Unit, std::string> applied = slot->SetEgressPolicy(policy);
        if (!applied.ok()) {
            return core::make_unexpected(
                core::err("configure sandbox network policy: " + applied.error()));
        }
    } else if (policy.has_value()) {
        return core::make_unexpected(core::err("sandbox has no active network slot"));
    }
    // Rust `current_network_policy`, which a later capture persists as
    // `snapshot_common.network_policy`.
    impl_->snapshot_config.common.network_policy = policy;
    return core::Unit{};
}
void FirecrackerSandboxBackend::UpdateCustomExtensionParams(
    const core::Optional<std::string>& /*params*/) {
    // Rust's signature returns nothing, so there is no way to report that this
    // is unported. Left as a no-op deliberately: the params only affect a
    // later capture, and every capture path in this build already fails.
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
