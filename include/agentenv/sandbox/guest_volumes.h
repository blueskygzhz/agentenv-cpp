// SPDX-License-Identifier: MIT
// Rust: src/sandbox/guest_volumes.rs — guest-side volume filesystem
// operations run through envd.
//
// Every function here turns a host-side decision into a command that runs
// *inside* the VM. That is the whole risk profile of the file: the commands
// act on the guest's mount namespace, where `mount`, `umount` and `fsfreeze`
// can each be pointed at the wrong filesystem by a race, and one of them
// (`fsfreeze`) can wedge the filesystem that hosts envd itself. The ordering
// and the validation below are what keep that from happening; they are not
// defensive decoration.
//
// Port shape: upstream takes an `EnvdInstance` and immediately wraps it in
// `Executor::new(envd).with_root_user()`. The seam that actually matters is
// the executor, so these take a `sandbox::Executor*` directly — the ported
// `Executor` interface is exactly upstream's post-`with_root_user()` surface
// (`run_command_with_opts` + `create_dir_all`). Two consequences:
//
//   * Selecting the root identity is the *caller's* obligation. `mount`,
//     `umount`, `fsfreeze` and `mkdir -p /run/...` all require it, and a
//     non-root executor turns every call here into EPERM rather than into a
//     diagnosable failure.
//   * `create_dir_all` goes through envd's filesystem service upstream, not
//     through an exec'd `mkdir`, so it works in images with no userland. A
//     caller that substitutes a command-based executor changes that property.
//
// Also deliberately not ported: upstream's `spawn_blocking` +
// `Handle::block_on` wrappers on the mount and sync entry points. They exist
// to move a blocking gRPC call off the async worker; these calls already
// block, so there is nothing to move. The two upstream `.context(...)`
// strings that describe a *join* failure ("initial volume mount task failed",
// "volume filesystem flush task failed") therefore have no counterpart —
// reproducing them would attribute an error to a task boundary that does not
// exist here.
#ifndef AGENTENV_SANDBOX_GUEST_VOLUMES_H_
#define AGENTENV_SANDBOX_GUEST_VOLUMES_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/process.h"

namespace agentenv {
namespace sandbox {

/// Rust `/agentenv/bin/busybox` — every guest command here is a busybox
/// applet, never the image's own `/bin/mount`.
///
/// Load-bearing: the drives being mounted may *be* the image's userland, and
/// a scratch or distroless guest has no mount binary at all. Resolving these
/// through `PATH` would make volume attachment depend on the customer image.
extern const char* const kGuestBusyboxPath;

/// Rust `tokio::time::timeout(Duration::from_secs(30), ..)` around the
/// freeze/thaw call.
const int64_t kVolumeFreezeTimeoutMs = 30 * 1000;

/// Rust element of `Vec<(usize, ExtraDrive)>`.
///
/// The index is not decoration: it is the drive's position in the sandbox's
/// extra-drive list, which is what fixes its guest device name. Pairing it
/// with the drive keeps a filtered subset (upstream mounts only the drives
/// init did not) from renumbering the devices.
struct IndexedGuestDrive {
    std::size_t index;
    ExtraDrive  drive;

    IndexedGuestDrive() : index(0) {}
    IndexedGuestDrive(std::size_t i, const ExtraDrive& d) : index(i), drive(d) {}
};

/// Rust `format!("/dev/vd{}", char::from(b'c' + index as u8))`.
///
/// `vda` is the root filesystem and `vdb` the tools drive, so extra drives
/// start at `vdc`. The ceiling (`vdz`, 24 drives) is enforced where the drive
/// set is built — `ValidateExtraDriveSet` and `BuildDrivesBootArg` — and is
/// deliberately *not* re-derived here. A second copy of the limit is how the
/// two drift, and the boot argument and this naming have to agree exactly:
/// they are the two halves of one device-to-mount-point mapping.
std::string GuestDeviceForDriveIndex(std::size_t index);

/// Rust `set_volume_freeze`'s inline shell script.
///
/// Exposed so a test can pin the two properties that make it safe (see
/// `SetVolumeFreeze`) instead of only observing their effects, which would
/// require a real frozen filesystem.
const char* VolumeFreezeScript();

/// Rust `mount_initial_guest_drives`.
///
/// Mounts the drives a snapshot's captured mount namespace does not already
/// cover. Sequential and fail-fast: a later drive's mount may target a path
/// under an earlier one, so running these concurrently would make the result
/// depend on completion order.
core::Expected<core::Unit, std::string> MountInitialGuestDrives(
    Executor* guest, const std::vector<IndexedGuestDrive>& drives);

/// Rust `sync_writable_volume_filesystems` — `busybox sync` before a pause.
///
/// A non-zero exit is an error, not a warning. The capture that follows seals
/// the volume's backing device; unflushed page cache would be captured as a
/// volume missing writes the guest believes it made.
core::Expected<core::Unit, std::string> SyncWritableVolumeFilesystems(Executor* guest);

/// Rust `set_volume_freeze`.
///
/// `frozen_device` *present* selects `--unfreeze` and carries the device
/// number recorded when the freeze succeeded; absent selects `--freeze`.
/// Presence and emptiness are distinct and must stay so: the caller pushes
/// the thaw obligation with an empty device string *before* awaiting the
/// freeze, so an empty recorded device means "the freeze's outcome is
/// unknown". Treating it as "no device to check" would let the thaw's
/// `fsfreeze --unfreeze` run against whatever is mounted at that path now.
///
/// Returns the command's output rather than mapping a non-zero exit to an
/// error, because the caller needs more than success: stdout carries the
/// frozen device number it must record to thaw later, and the two callers
/// classify a failed freeze differently (terminal) from a failed thaw.
core::Expected<ProcessOutput, std::string> SetVolumeFreeze(
    Executor* guest, const std::string& mount,
    const core::Optional<std::string>& frozen_device);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_GUEST_VOLUMES_H_
