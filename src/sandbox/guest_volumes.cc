// SPDX-License-Identifier: MIT
// Rust: src/sandbox/guest_volumes.rs
#include "agentenv/sandbox/guest_volumes.h"

#include "agentenv/core/logging.h"

namespace agentenv {
namespace sandbox {

const char* const kGuestBusyboxPath = "/agentenv/bin/busybox";

namespace {

/// Rust `ProcessOpts::default().with_cwd("/")`.
///
/// The `/` is not a formality. envd's default workdir comes from the image and
/// can sit *inside* a path this file is about to mount over, bind from, or
/// lazily unmount; a process whose cwd is in there holds that directory and
/// turns the unmount into EBUSY. `/` is the one directory no volume operation
/// can be targeting.
ProcessOpts GuestOpts() {
    ProcessOpts opts;
    opts.WithCwd("/");
    return opts;
}

/// Rust `run_guest_command`.
///
/// Upstream threads the command through as a parameter, but every call site
/// passes busybox; the constant is folded in so no caller can route one of
/// these operations through the image's own binaries.
core::Expected<ProcessOutput, std::string> RunGuestCommand(
    Executor* guest, const std::vector<std::string>& args) {
    return guest->RunCommandWithOpts(kGuestBusyboxPath, args, GuestOpts());
}

/// Rust `output.stderr.trim()`.
///
/// Lossy-decoded first: upstream's `ProcessOutput::stderr` is already a
/// `String` because the conversion happened at the envd boundary, while this
/// port carries the raw bytes, and `TrimUnicodeWhitespace` is defined only on
/// valid UTF-8. The composition reproduces the end-to-end upstream result.
std::string TrimmedStderr(const ProcessOutput& output) {
    return core::TrimUnicodeWhitespace(core::FromUtf8Lossy(output.stderr_data));
}

/// Rust's single-element `vec!["umount", "-l", path]` guest command, whose
/// result is discarded at both call sites.
void LazyUnmountIgnoringFailure(Executor* guest, const std::string& path) {
    std::vector<std::string> args;
    args.push_back("umount");
    args.push_back("-l");
    args.push_back(path);
    RunGuestCommand(guest, args);
}

/// Rust `mount_guest_path_with_options`.
core::Expected<core::Unit, std::string> MountGuestPathWithOptions(
    Executor* guest, const std::vector<std::string>& options, const std::string& source,
    const std::string& target) {
    // `-n`: do not write /etc/mtab. The guest's /etc may live on a drive this
    // very loop is still mounting, and a mount that succeeds in the kernel but
    // fails to record itself would be reported as a failure and then retried
    // against an already-mounted target.
    std::vector<std::string> args;
    args.push_back("mount");
    args.push_back("-n");
    for (std::size_t i = 0; i < options.size(); ++i) args.push_back(options[i]);
    args.push_back(source);
    args.push_back(target);

    const core::Expected<ProcessOutput, std::string> first = RunGuestCommand(guest, args);
    if (!first.ok()) return core::make_unexpected(first.error());
    if (first.value().exit_code == 0) return core::Unit();

    // A stale snapshot mount must not be accepted just because its target
    // matches. The guest may have resumed with something already mounted here
    // — a previous incarnation's device, or the same path from the captured
    // namespace — and treating "already mounted" as success would hand the
    // sandbox a filesystem backed by the wrong device while every path lookup
    // appeared to work. So the target is cleared and the *requested*
    // source/options are retried once.
    //
    // The unmount is lazy and its failure ignored: if there was nothing
    // mounted there the umount fails harmlessly, and the retry's own exit
    // status is the authoritative answer either way.
    LazyUnmountIgnoringFailure(guest, target);

    const core::Expected<ProcessOutput, std::string> retry = RunGuestCommand(guest, args);
    if (!retry.ok()) return core::make_unexpected(retry.error());
    if (retry.value().exit_code == 0) return core::Unit();

    return core::make_unexpected(std::string("failed to mount ") + source + " at " + target +
                                 " during snapshot launch (exit code " +
                                 std::to_string(retry.value().exit_code) + "): " +
                                 TrimmedStderr(retry.value()));
}

/// Rust `create_guest_directory`.
core::Expected<core::Unit, std::string> CreateGuestDirectory(Executor* guest,
                                                             const std::string& path) {
    return guest->CreateDirAll(path);
}

}  // namespace

std::string GuestDeviceForDriveIndex(std::size_t index) {
    const char letter = static_cast<char>('c' + static_cast<unsigned char>(index));
    return std::string("/dev/vd") + letter;
}

const char* VolumeFreezeScript() {
    // Reproduced verbatim from upstream's raw string literal so the two can be
    // diffed character for character. Two properties carry the safety of this
    // whole operation:
    //
    //   * `exec 3< "$1"` opens the mount point *once* and both the device
    //     check and the ioctl then address `/proc/self/fd/3`. If the script
    //     instead stat'd the path and then froze the path, an unmount landing
    //     between the two would redirect `fsfreeze` at whatever is now at that
    //     path — in the worst case the root filesystem hosting envd, which
    //     would freeze the daemon that is supposed to thaw it. There is no
    //     recovery from that short of killing the VM.
    //   * The validation is asymmetric on purpose. Freezing refuses a device
    //     that *is* the root device (same reason as above). Thawing instead
    //     requires the device to equal the one recorded at freeze time: a
    //     thaw aimed at a filesystem nobody froze is a silent no-op today and
    //     an unbalanced thaw of someone else's freeze tomorrow.
    //
    // `stat -Lc %d` follows symlinks for the mount point; the root comparison
    // uses `-c` on `/` where there is nothing to follow.
    return "\n"
           "        set -eu\n"
           "        exec 3< \"$1\"\n"
           "        device=$(/agentenv/bin/busybox stat -Lc %d /proc/self/fd/3)\n"
           "        if [ \"$2\" = --freeze ]; then\n"
           "            test \"$device\" != \"$(/agentenv/bin/busybox stat -c %d /)\"\n"
           "        else\n"
           "            test \"$device\" = \"$3\"\n"
           "        fi\n"
           "        /agentenv/bin/busybox fsfreeze \"$2\" /proc/self/fd/3\n"
           "        printf '%s\\n' \"$device\"\n"
           "    ";
}

core::Expected<core::Unit, std::string> MountInitialGuestDrives(
    Executor* guest, const std::vector<IndexedGuestDrive>& drives) {
    for (std::size_t i = 0; i < drives.size(); ++i) {
        const ExtraDrive& drive = drives[i].drive;
        const std::string device = GuestDeviceForDriveIndex(drives[i].index);
        const std::string target = drive.mount_path;

        const core::Expected<core::Unit, std::string> dir = CreateGuestDirectory(guest, target);
        if (!dir.ok()) return core::make_unexpected(dir.error());

        std::vector<std::string> mount_options;
        if (drive.read_only) {
            // Volume snapshots are crash-consistent: even after sync, ext4
            // retains the needs-recovery flag until a clean unmount, and the
            // guest never gets one. A read-only OverlayBD device cannot
            // replay that journal, so without `noload` the mount fails
            // outright; without `ro` the kernel would be free to attempt a
            // write to a device that cannot take one. Both halves are needed.
            mount_options.push_back("-o");
            mount_options.push_back("ro,noload");
        }

        if (!drive.sub_path.has_value()) {
            const core::Expected<core::Unit, std::string> mounted =
                MountGuestPathWithOptions(guest, mount_options, device, target);
            if (!mounted.ok()) return core::make_unexpected(mounted.error());
            continue;
        }

        // A `sub_path` cannot be mounted directly: the kernel mounts a whole
        // filesystem, so the drive is first staged somewhere the guest does
        // not look, and only the requested subtree is bound onto the target.
        // The stage lives under /run, keyed by drive id so two drives
        // mounting different sub-paths cannot collide on it.
        const std::string stage = std::string("/run/agentenv-drive-") + drive.drive_id;
        const core::Expected<core::Unit, std::string> stage_dir =
            CreateGuestDirectory(guest, stage);
        if (!stage_dir.ok()) return core::make_unexpected(stage_dir.error());

        const core::Expected<core::Unit, std::string> staged =
            MountGuestPathWithOptions(guest, mount_options, device, stage);
        if (!staged.ok()) return core::make_unexpected(staged.error());

        // Joined raw, exactly as upstream: `sub_path` was validated against
        // escaping its drive root by `ValidateSubPath` when the drive was
        // built. Re-sanitising here would be a second, divergent rule for
        // what a sub-path may contain.
        const std::string source = stage + "/" + *drive.sub_path;
        std::vector<std::string> bind_options;
        bind_options.push_back("--bind");
        const core::Expected<core::Unit, std::string> bound =
            MountGuestPathWithOptions(guest, bind_options, source, target);
        if (!bound.ok()) return core::make_unexpected(bound.error());

        // Detach the stage so the full filesystem is not reachable outside
        // the requested sub-path. Lazy, and the failure is ignored on
        // purpose: the bind holds a reference to the filesystem, so the
        // unmount of the stage path cannot complete immediately and *must
        // not* be waited for or treated as fatal — the mount the caller asked
        // for has already succeeded, and failing here would report a mounted
        // volume as unmounted.
        LazyUnmountIgnoringFailure(guest, stage);
    }
    return core::Unit();
}

core::Expected<core::Unit, std::string> SyncWritableVolumeFilesystems(Executor* guest) {
    std::vector<std::string> args;
    args.push_back("sync");
    const core::Expected<ProcessOutput, std::string> output = RunGuestCommand(guest, args);
    if (!output.ok()) return core::make_unexpected(output.error());
    if (output.value().exit_code != 0) {
        return core::make_unexpected(
            std::string("failed to flush writable volume filesystems before pause (exit code ") +
            std::to_string(output.value().exit_code) + "): " + TrimmedStderr(output.value()));
    }
    return core::Unit();
}

core::Expected<ProcessOutput, std::string> SetVolumeFreeze(
    Executor* guest, const std::string& mount,
    const core::Optional<std::string>& frozen_device) {
    // Upstream's `mount.to_str().context("volume mount path is not valid
    // UTF-8")` has no counterpart: the path is already a byte string here, and
    // the guest command line takes bytes.
    std::vector<std::string> args;
    args.push_back("sh");
    args.push_back("-c");
    args.push_back(VolumeFreezeScript());
    // `$0` for the script. Named rather than left empty so a guest-side
    // diagnostic from `set -eu` attributes itself to this operation.
    args.push_back("freeze-volume");
    args.push_back(mount);
    args.push_back(frozen_device.has_value() ? "--unfreeze" : "--freeze");
    // Rust `frozen_device.unwrap_or_default()` — `$3` is always supplied, so
    // the script's `test "$device" = "$3"` never reads an unset parameter
    // under `set -u`. On the freeze path it is unused.
    args.push_back(frozen_device.has_value() ? *frozen_device : std::string());

    ProcessOpts opts = GuestOpts();
    // Upstream bounds the whole call with `tokio::time::timeout(30s)`, which
    // abandons the request. There is nothing to abandon a blocking call with
    // here, so the bound is pushed into the guest as a process timeout. The
    // residual difference is deliberate and worth stating: envd kills the
    // applet at the deadline rather than the host walking away from it, so a
    // wedged `fsfreeze` is reaped instead of left running in the guest — but a
    // wedged *transport* is no longer bounded by this, only by the executor's
    // own limits. Dropping the bound entirely was the one unacceptable option:
    // a freeze that never returns blocks the capture holding the sandbox lock.
    opts.WithTimeoutMs(kVolumeFreezeTimeoutMs);

    return guest->RunCommandWithOpts(kGuestBusyboxPath, args, opts);
}

}  // namespace sandbox
}  // namespace agentenv
