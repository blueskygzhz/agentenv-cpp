// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs ::
//   FirecrackerSandbox::{configure_microvm, configure_extra_drives}
//
// The pre-boot call sequence. The port already had every individual API call
// (instance.h) but nothing that ordered them, which is the part that decides
// whether a VM boots at all rather than whether one request is well-formed.
//
// Why this is a plan instead of a method that issues the calls: the ordering is
// the fragile part and it is what a test needs to see. A wrong order still
// yields well-formed requests that Firecracker accepts individually, so the
// failure surfaces much later -- as a guest that mounts the wrong filesystem,
// or a `PUT` refused because the VM has already booted. Building the sequence
// as data lets those rules be asserted without a VMM, a work dir or KVM.
#include "agentenv/sandbox/firecracker/sandbox.h"

#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/sandbox/firecracker/lifecycle.h"
// `ResumeStep` / `ResumeStartSpec`. The resume sequence is built here next to
// the configure one: both are ordered API plans over the same instance, and
// the reason for modelling them as data is identical.
#include "agentenv/sandbox/firecracker/sandbox_backend.h"

#include <cstdio>
#include <utility>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

ConfigureStep DriveStep(const std::string& drive_id, const std::string& path_on_host,
                        bool is_root, bool read_only, bool direct, IoEngine io_engine,
                        const core::Optional<RateLimiter>& rate_limiter) {
    ConfigureStep step;
    step.kind = ConfigureStep::Kind::Drive;
    step.id = drive_id;
    step.path = path_on_host;
    step.is_root = is_root;
    step.read_only = read_only;
    step.direct = direct;
    step.io_engine = io_engine;
    step.rate_limiter = rate_limiter;
    return step;
}

/// Rust `configure_extra_drives` — every extra drive is non-root, direct, and
/// Async, and carries no limiter. Only `read_only` varies, per mount.
void AppendExtraDrives(const std::vector<DriveMount>& mounts,
                       std::vector<ConfigureStep>* out) {
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        out->push_back(DriveStep(mounts[i].drive_id, mounts[i].attachment_path,
                                 /*is_root=*/false, mounts[i].read_only, /*direct=*/true,
                                 IoEngine::Async, core::nullopt));
    }
}

/// Names a step for an error message. The failing call is the first thing an
/// operator needs, and the index alone does not survive a reordering.
std::string ConfigureStepName(const ConfigureStep& step) {
    switch (step.kind) {
    case ConfigureStep::Kind::Logger:           return "PUT /logger";
    case ConfigureStep::Kind::MachineConfig:    return "PUT /machine-config";
    case ConfigureStep::Kind::CpuConfig:        return "PUT /cpu-config";
    case ConfigureStep::Kind::BootSource:       return "PUT /boot-source";
    case ConfigureStep::Kind::Drive:            return "PUT /drives/" + step.id;
    case ConfigureStep::Kind::NetworkInterface: return "PUT /network-interfaces/" + step.id;
    case ConfigureStep::Kind::MmdsConfig:       return "PUT /mmds/config";
    case ConfigureStep::Kind::Mmds:             return "PUT /mmds";
    case ConfigureStep::Kind::Balloon:          return "PUT /balloon";
    }
    return "unknown step";
}

}  // namespace

core::Expected<std::vector<ConfigureStep>, std::string>
BuildConfigureSequence(const ConfigurePlan& plan) {
    std::vector<ConfigureStep> steps;

    // 1. Logger, first. Firecracker accepts it pre-boot and only once, so it
    //    has to precede anything that could fail: a later failure is the thing
    //    the log was wanted for.
    //
    //    A blank level means logging is off (`LoggingEnabled`), so a
    //    whitespace-only config value does not create an empty log file.
    if (LoggingEnabled(plan.log_level.empty() ? core::nullopt
                                              : core::Optional<std::string>(plan.log_level)) &&
        !plan.log_path.empty()) {
        ConfigureStep step;
        step.kind = ConfigureStep::Kind::Logger;
        step.path = plan.log_path;
        step.text = plan.log_level;
        steps.push_back(step);
    }

    // 2. Machine config. `smt` is hard-coded false, as Rust passes.
    {
        ConfigureStep step;
        step.kind = ConfigureStep::Kind::MachineConfig;
        step.mem_size_mib = plan.mem_size_mib;
        step.vcpu_count = plan.vcpu_count;
        step.smt = false;
        step.track_dirty_pages = plan.track_dirty_pages;
        steps.push_back(step);
    }

    // 3. CPU template, only when non-empty. Rust checks `!cpu_json.is_empty()`
    //    inside the `Some` arm, so a present-but-empty string is not a request
    //    for an empty template -- Firecracker would reject that body.
    if (!plan.cpu_config_json.empty()) {
        ConfigureStep step;
        step.kind = ConfigureStep::Kind::CpuConfig;
        step.text = plan.cpu_config_json;
        steps.push_back(step);
    }

    // 4. Boot source. `initrd_path` is always absent on this path.
    {
        ConfigureStep step;
        step.kind = ConfigureStep::Kind::BootSource;
        step.path = plan.kernel_image;
        if (plan.boot_args.has_value()) step.text = *plan.boot_args;
        steps.push_back(step);
    }

    // 5. Drives, in guest slot order. The guest names virtio-blk devices
    //    positionally (`vda`, `vdb`, `vdc`...), so the order here *is* the
    //    guest's device layout. Attaching these out of order still succeeds at
    //    the API and then mounts the wrong filesystem.

    // vda: tools drive, root device, always read-only. Sync engine and no
    // limiter -- it is small, read-only and on the boot path.
    steps.push_back(DriveStep(kRootfsDriveId, kRootfsDrivePath, /*is_root=*/true,
                              /*read_only=*/true, /*direct=*/false, IoEngine::Sync,
                              core::nullopt));

    // vdb: user image, writable. The limiter is attached here as pre-boot
    // drive config rather than PATCHed after start, so throttling is in force
    // from the guest's first I/O instead of after a brief unthrottled window.
    const core::Expected<core::Optional<RateLimiter>, std::string> limiter =
        BuildDiskRateLimiter(plan.disk_rate_limit);
    if (!limiter.ok()) return core::make_unexpected(limiter.error());
    steps.push_back(DriveStep(kUserRootfsDriveId, kUserRootfsDrivePath, /*is_root=*/false,
                              /*read_only=*/false, /*direct=*/true, IoEngine::Async,
                              limiter.value()));

    // vdc+: physical extra drives, then the reserved volume slots.
    //
    // Rust splits one prepared list at `physical_extra_drive_count`. Splitting
    // a shorter list would silently reclassify a physical drive as a volume
    // slot, so a short list is an error rather than a truncation.
    if (plan.physical_extra_drive_count > plan.extra_drive_attachments.size()) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "fresh sandbox expects %zu physical extra drives, but only %zu were "
                      "prepared",
                      plan.physical_extra_drive_count, plan.extra_drive_attachments.size());
        return core::make_unexpected(std::string(msg));
    }
    const std::size_t physical_count = plan.physical_extra_drive_count;
    const std::vector<DriveMount> physical(plan.extra_drive_attachments.begin(),
                                           plan.extra_drive_attachments.begin() +
                                               static_cast<std::ptrdiff_t>(physical_count));
    const std::vector<DriveMount> volumes(plan.extra_drive_attachments.begin() +
                                              static_cast<std::ptrdiff_t>(physical_count),
                                          plan.extra_drive_attachments.end());
    AppendExtraDrives(physical, &steps);

    // Every volume occupies a slot reserved on the first boot, because
    // Firecracker cannot add a virtio-block device after a snapshot restore.
    // More volumes than slots therefore cannot be fixed later by adding one:
    // it is a permanent misconfiguration and must fail now.
    if (volumes.size() > plan.volume_drive_slots) {
        return core::make_unexpected(
            std::string("fresh sandbox has more volume drives than reserved volume slots"));
    }
    // The slots are attached by slot id, not by the volume's own id: the guest
    // must see the same device names across a restore, and the binding from
    // slot to volume is a symlink made before this point.
    std::vector<DriveMount> slots;
    for (std::size_t i = 0; i < plan.volume_drive_slots; ++i) {
        DriveMount slot;
        slot.drive_id = VolumeDriveSlotId(physical_count + i);
        slot.attachment_path = slot.drive_id + ".img";
        slot.read_only = false;
        slots.push_back(slot);
    }
    AppendExtraDrives(slots, &steps);

    // 6. Network, MMDS config and the MMDS document -- all three together, or
    //    none. MMDS is served over the interface, so configuring the document
    //    without one leaves the guest unable to read its own metadata.
    if (plan.has_network_slot) {
        ConfigureStep iface;
        iface.kind = ConfigureStep::Kind::NetworkInterface;
        iface.id = "eth0";
        iface.path = "tap0";
        steps.push_back(iface);

        ConfigureStep mmds_cfg;
        mmds_cfg.kind = ConfigureStep::Kind::MmdsConfig;
        mmds_cfg.id = "eth0";
        steps.push_back(mmds_cfg);

        ConfigureStep mmds;
        mmds.kind = ConfigureStep::Kind::Mmds;
        mmds.text = plan.mmds_json;
        steps.push_back(mmds);
    }

    // 7. Balloon, last. Paired with DAMON reclaim in the kernel boot args:
    //    DAMON reclaims cold pagecache inside the guest and the balloon reports
    //    the freed pages to the host, so the host can release the memory.
    {
        ConfigureStep step;
        step.kind = ConfigureStep::Kind::Balloon;
        steps.push_back(step);
    }

    return steps;
}

core::Expected<core::Unit, std::string>
ApplyConfigureSequence(FirecrackerInstance* instance,
                       const std::vector<ConfigureStep>& steps) {
    if (instance == NULL) {
        return core::make_unexpected(std::string("no firecracker instance"));
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ConfigureStep& step = steps[i];
        core::Expected<core::Unit, std::string> r = core::Unit{};
        switch (step.kind) {
        case ConfigureStep::Kind::Logger:
            r = instance->SetLogger(step.path, step.text);
            break;
        case ConfigureStep::Kind::MachineConfig:
            r = instance->SetMachineConfig(step.mem_size_mib, step.vcpu_count, step.smt,
                                           step.track_dirty_pages);
            break;
        case ConfigureStep::Kind::CpuConfig:
            r = instance->SetCpuConfig(step.text);
            break;
        case ConfigureStep::Kind::BootSource:
            // `initrd_path` is absent on this path; empty boot args stay absent
            // rather than being sent as an empty string.
            r = instance->SetBootSource(
                step.path, core::nullopt,
                step.text.empty() ? core::nullopt : core::Optional<std::string>(step.text));
            break;
        case ConfigureStep::Kind::Drive:
            r = instance->AddDrive(step.id, step.path, step.is_root, step.read_only,
                                   step.direct, step.io_engine, step.rate_limiter);
            break;
        case ConfigureStep::Kind::NetworkInterface:
            r = instance->AddNetworkInterface(step.id, core::nullopt, step.path, core::nullopt,
                                              core::nullopt);
            break;
        case ConfigureStep::Kind::MmdsConfig:
            r = instance->SetMmdsConfig(step.id);
            break;
        case ConfigureStep::Kind::Mmds: {
            // The plan carries the document serialized. Parse it back through
            // `FromValue` rather than inventing a raw-text path: that keeps the
            // reserved-key filter and the four required fields in force, which
            // is the whole reason MmdsMetadata is a class and not a string.
            const core::Expected<core::Json, core::AnyError> doc =
                core::Json::Parse(step.text);
            if (!doc.ok()) {
                return core::make_unexpected(std::string("PUT /mmds: ") +
                                             doc.error().chain());
            }
            const core::Expected<MmdsMetadata, std::string> metadata =
                MmdsMetadata::FromValue(doc.value());
            if (!metadata.ok()) {
                return core::make_unexpected(std::string("PUT /mmds: ") + metadata.error());
            }
            r = instance->SetMmds(metadata.value());
            break;
        }
        case ConfigureStep::Kind::Balloon:
            r = instance->SetBalloon();
            break;
        }
        if (!r.ok()) {
            // Abort the rest. These are pre-boot calls and a later one cannot
            // repair an earlier rejection, so continuing would only replace a
            // precise error with a confusing one from further down.
            return core::make_unexpected(ConfigureStepName(step) + ": " + r.error());
        }
    }
    return core::Unit{};
}

// ── resume sequence ─────────────────────────────────────────────────────────

namespace {

std::string ResumeStepName(const ResumeStep& step) {
    switch (step.kind) {
    case ResumeStep::Kind::Logger:           return "PUT /logger";
    case ResumeStep::Kind::SnapshotLoad:     return "PUT /snapshot/load";
    case ResumeStep::Kind::PatchDrivePath:   return "PATCH /drives/" + step.id + " (path)";
    case ResumeStep::Kind::Mmds:             return "PUT /mmds";
    case ResumeStep::Kind::PatchRateLimiter: return "PATCH /drives/" + step.id + " (limiter)";
    case ResumeStep::Kind::Resume:           return "PATCH /vm (Resumed)";
    }
    return "unknown resume step";
}

}  // namespace

core::Expected<std::vector<ResumeStep>, std::string>
BuildResumeSequence(const ResumeStartSpec& spec) {
    const SnapshotConfig& config = spec.snapshot_config;
    std::vector<ResumeStep> steps;

    // Checked before anything is created. Each of these would otherwise
    // surface as a VM that loaded and then could not find its disk, by which
    // point the error names the load rather than the missing field.
    if (!config.common.rootfs_image_config.has_value()) {
        return core::make_unexpected(std::string("snapshot rootfs image config is missing"));
    }
    if (!config.common.rootfs_virtual_size.has_value()) {
        return core::make_unexpected(std::string("snapshot rootfs virtual size is missing"));
    }
    if (config.vm_state_path.empty()) {
        return core::make_unexpected(std::string("snapshot vm state path is missing"));
    }
    // Rust fails fast on `UblkDeviceManager::is_available()` for exactly this:
    // memory restore reads through a block device, and without one the load
    // would be issued against a path that does not exist.
    if (spec.mem_device_path.empty()) {
        return core::make_unexpected(std::string(
            "snapshot resume requires a memory block device "
            "(this build has no ublk daemon client)"));
    }

    // 1. Logger first. Firecracker accepts `PUT /logger` pre-boot only, and a
    //    loaded snapshot is no longer pre-boot.
    if (LoggingEnabled(config.common.firecracker_log_level)) {
        ResumeStep logger;
        logger.kind = ResumeStep::Kind::Logger;
        logger.path = "firecracker.log";  // resolved against the work dir
        logger.text = *config.common.firecracker_log_level;
        steps.push_back(logger);
    }

    // 2. The load itself.
    ResumeStep load;
    load.kind = ResumeStep::Kind::SnapshotLoad;
    load.path = config.vm_state_path;
    load.text = spec.mem_device_path;
    load.track_dirty_pages = config.common.track_dirty_pages;
    steps.push_back(load);

    // 3. Volumes absent from the source state were loaded as placeholders, so
    //    their real paths are patched in after the load. A volume that *was*
    //    in the source state is bound before the load instead and needs no
    //    patch -- this port binds none, so only the shape is modelled here.
    const std::size_t physical = config.common.physical_extra_drive_count;
    for (std::size_t i = physical; i < config.common.extra_drives.size(); ++i) {
        ResumeStep patch;
        patch.kind = ResumeStep::Kind::PatchDrivePath;
        patch.id = VolumeDriveSlotId(i - physical);
        patch.path = config.common.extra_drives[i].image_config_path;
        steps.push_back(patch);
    }

    // 4. MMDS before the resume: the guest reads its identity from the
    //    document, and `PrepareSnapshotConfigForResume` has just corrected it.
    //    Letting the guest run first would let it read the captured id.
    if (config.common.mmds_metadata.has_value()) {
        ResumeStep mmds;
        mmds.kind = ResumeStep::Kind::Mmds;
        mmds.text = config.common.mmds_metadata->ToJson();
        steps.push_back(mmds);
    }

    // 5. Reconcile the inherited rate limiter while the VM is loaded but still
    //    paused. After the resume the guest has already issued I/O under
    //    whatever limit the capture carried; before the load there is no drive
    //    to patch. Both buckets are always written, so a dimension this node
    //    leaves unset clears the inherited one rather than keeping it.
    const core::Expected<RateLimiter, std::string> reconciled =
        ReconcileDiskRateLimiter(config.common.disk_rate_limit);
    if (!reconciled.ok()) return core::make_unexpected(reconciled.error());
    ResumeStep limiter;
    limiter.kind = ResumeStep::Kind::PatchRateLimiter;
    limiter.id = kUserRootfsDriveId;
    limiter.rate_limiter = reconciled.value();
    steps.push_back(limiter);

    // 6. Let it run.
    ResumeStep resume;
    resume.kind = ResumeStep::Kind::Resume;
    steps.push_back(resume);

    return steps;
}

core::Expected<core::Unit, std::string>
ApplyResumeSequence(FirecrackerInstance* instance, const std::vector<ResumeStep>& steps) {
    if (instance == NULL) {
        return core::make_unexpected(std::string("no firecracker instance"));
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ResumeStep& step = steps[i];
        core::Expected<core::Unit, std::string> r = core::Unit{};
        switch (step.kind) {
        case ResumeStep::Kind::Logger:
            r = instance->SetLogger(step.path, step.text);
            break;
        case ResumeStep::Kind::SnapshotLoad:
            // `resume_vm = false`: the VM is left loaded-but-paused so the
            // limiter can be reconciled before the guest issues any I/O.
            r = instance->LoadSnapshotFile(
                step.path, step.text,
                std::vector<std::pair<std::string, std::string> >(
                    1, std::make_pair(std::string("eth0"), std::string("tap0"))),
                /*resume_vm=*/false, step.track_dirty_pages);
            break;
        case ResumeStep::Kind::PatchDrivePath:
            r = instance->PatchDrivePath(step.id, step.path);
            break;
        case ResumeStep::Kind::Mmds: {
            const core::Expected<core::Json, core::AnyError> doc =
                core::Json::Parse(step.text);
            if (!doc.ok()) {
                return core::make_unexpected(std::string("PUT /mmds: ") + doc.error().chain());
            }
            const core::Expected<MmdsMetadata, std::string> metadata =
                MmdsMetadata::FromValue(doc.value());
            if (!metadata.ok()) {
                return core::make_unexpected(std::string("PUT /mmds: ") + metadata.error());
            }
            r = instance->SetMmds(metadata.value());
            break;
        }
        case ResumeStep::Kind::PatchRateLimiter:
            r = instance->PatchDriveRateLimiter(step.id, step.rate_limiter);
            break;
        case ResumeStep::Kind::Resume:
            r = instance->Resume();
            break;
        }
        if (!r.ok()) {
            // Abort the rest, as the fresh path does: once the load is
            // rejected there is no VM for the later calls to act on, and a
            // patch against a nonexistent drive reports a confusing error.
            return core::make_unexpected(ResumeStepName(step) + ": " + r.error());
        }
    }
    return core::Unit{};
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
