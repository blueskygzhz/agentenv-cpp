// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs —
//   capture_live_overlaybd_snapshot, restack_target_upper_data_path,
//   rewrite_live_runtime_config_for_restack,
//   stage_overlaybd_snapshot_from_live_runtime,
//   restack_snapshot_overlaybd_device, restack_snapshot_overlaybd_rootfs,
//   link_or_copy_runtime_layer, canonicalized_runtime_owned_roots.
//
// The **capture** half of an overlaybd snapshot, as opposed to the layer-stack
// bookkeeping in `mem_image_config.h`. Taking a snapshot of a *writable*
// overlaybd device means asking the ublk daemon to seal the live upper into a
// new sealed layer and reopen a fresh empty upper above it ("restacking"), then
// writing a persisted `image.json` that names the resulting stack.
//
// Why the error classification here is the whole point
// ----------------------------------------------------
//
// The seal mutates the live runtime. Once the daemon has sealed the old upper
// and reopened a new one, the live image config on disk no longer describes the
// device the guest is running on: the sealed layer has to be appended to its
// lowers for the two to agree again. Every step after the seal is therefore
// `Terminal` on failure -- not because the snapshot is unusable, but because
// the *source sandbox* is. A caller that read one of those failures as
// recoverable and resumed the guest would be running a VM whose block device
// silently lost the contents of the sealed layer.
//
// Conversely, everything *before* the seal is `Recoverable`: nothing has been
// touched, so the sandbox is exactly as it was. The read-only path never seals
// at all, which is why it stays recoverable the whole way through.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_OVERLAYBD_RESTACK_H_
#define AGENTENV_SANDBOX_FIRECRACKER_OVERLAYBD_RESTACK_H_

#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/backend.h"
#include "agentenv/sandbox/firecracker/mem_image_config.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `MANAGED_BASE_LAYER_FILE` — where a compacted rootfs suffix lands.
extern const char* const kManagedBaseLayerFile;

/// Rust `overlaybd::LayerDescriptor` as the daemon reports it back.
///
/// Carried separately from `LayerConfig` because the daemon may legitimately
/// not report one (an older daemon omits the field), and the absence has to
/// stay distinguishable from a zero-sized layer: it decides whether the size
/// cross-check below can run at all.
struct RestackedLayer {
    std::string digest;
    uint64_t    size = 0;
};

/// Rust `UblkDeviceManager::global().restack_snapshot_device(device, path, kind)`.
///
/// Injected rather than called through a global, for two reasons. The global
/// device manager needs a live ublk daemon, so none of the ordering below is
/// reachable in a test without one; and the terminal/recoverable split this
/// header exists to enforce is driven by *what the daemon answered*, which is
/// exactly what a fake has to be able to vary.
///
/// An empty optional means the daemon sealed the layer but reported no
/// descriptor. That is a success, not a failure -- the size cross-check is
/// simply skipped.
typedef std::function<SandboxCaptureResult<core::Optional<RestackedLayer> >(
    const std::string& output_layer_path)>
    SnapshotRestacker;

/// Rust `local_layer_config_with_descriptor`.
///
/// With no descriptor this degrades to `LocalLayerConfig` (empty digest, zero
/// size), matching Rust's `unwrap_or_default`.
storage::overlaybd::LayerConfig LocalLayerConfigWithDescriptor(
    const std::string& path, const core::Optional<RestackedLayer>& descriptor);

/// Rust `restack_target_upper_data_path` — the live upper's backing file.
///
/// Fails when the live config carries no writable `upper.data`: a restack has
/// nothing to seal in that case, and proceeding would ask the daemon to seal a
/// read-only stack.
core::Expected<std::string, std::string> RestackTargetUpperDataPath(
    const std::string& live_runtime_image_config_path);

/// Rust `canonicalized_runtime_owned_roots`.
///
/// The roots under which a lower is considered runtime-owned -- i.e. a file
/// some later pause/fork/stop may delete -- so a snapshot must adopt or compact
/// it rather than reference it. A path that cannot be canonicalized is kept
/// verbatim, as Rust's `unwrap_or(root)` does; it may simply not exist yet.
///
/// Deviation: Rust memoizes this in a `OnceLock` and reads the two paths from
/// `ConfigManager::global_config()`. They are parameters here and nothing is
/// cached, so the comparison is testable without a global config. The cache was
/// only avoiding repeated `realpath` calls.
std::vector<std::string> CanonicalizedRuntimeOwnedRoots(
    const std::string& managed_snapshot_base, const std::string& persisted_sandbox_store_path);

/// Rust `link_or_copy_runtime_layer` / `link_or_copy_file_blocking` — the
/// production [`LayerAdopter`].
///
/// Hard-links when it can and falls back to a sparse copy when it cannot
/// (`cp --reflink=auto --sparse=always`), because the two files may be on
/// different filesystems. The fallback is a warning rather than an error: a
/// cross-device adoption is normal, not a fault.
///
/// A stale destination is removed first. Without that the hard link would fail
/// with EEXIST on a retry, turning an idempotent step into a one-shot one.
core::Expected<core::Unit, std::string> LinkOrCopyRuntimeLayer(const std::string& source,
                                                               const std::string& destination);

/// Rust `stage_overlaybd_snapshot_from_live_runtime` — write the persisted
/// `{output_dir}/image.json` describing the post-restack stack.
///
/// `snapshot_layer_path` is the sealed layer when one was produced, absent on
/// the read-only path. When present it is **verified to be the newest lower**
/// of the live config before being treated as the appended layer: the daemon
/// having sealed it is what put it there, so a mismatch means the live config
/// is not the one that was just restacked, and inheriting the wrong layer
/// would publish a stack that silently omits the captured writes.
///
/// The staged config drops the `upper` entirely and points `result_file` at
/// `./result.txt`: a persisted snapshot is read-only, and keeping the live
/// upper would make the snapshot share a writable file with the running
/// sandbox.
///
/// Returns a plain error string rather than a `SandboxCaptureError` because
/// staging cannot know whether it is terminal -- that depends on whether a
/// seal already happened, which only the caller knows.
core::Expected<std::string, std::string> StageOverlaybdSnapshotFromLiveRuntime(
    const std::string& live_runtime_image_config_path, const std::string& output_dir,
    const core::Optional<std::string>& snapshot_layer_path,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt);

/// Rust `restack_snapshot_overlaybd_device` — seal (unless read-only), then
/// stage. Returns the staged `image.json` path.
///
/// The ordering that carries consequences:
///
///   1. **Read-only short-circuits the seal.** There is no writable upper to
///      seal, so the staged config is built straight from the live one and the
///      whole operation stays recoverable.
///   2. **The sealed layer is produced on the destination's filesystem when
///      possible.** When it is not, the daemon writes it beside the live upper
///      and it is copied across afterwards -- the daemon can only seal into a
///      file on the device's own filesystem.
///   3. **That cross-filesystem copy is terminal on failure.** The seal has
///      already happened by then.
///   4. **A descriptor size mismatch is terminal and keeps the layer.** The
///      live device may still reference it, so deleting it would break the
///      running sandbox on top of the failed snapshot.
///   5. **Staging failure is terminal only if a seal happened** (Rust
///      `LiveOverlaybdSnapshotState::finish_staging`). On the read-only path
///      the same failure leaves the sandbox untouched.
SandboxCaptureResult<std::string> RestackSnapshotOverlaybdDevice(
    const SnapshotRestacker& restack, bool read_only,
    const std::string& live_runtime_image_config_path, const std::string& output_dir,
    const std::string& snapshot_layer_file_name,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt);

/// Rust `restack_snapshot_overlaybd_rootfs` — the rootfs call of the above,
/// into `{snapshot_root}/rootfs` with layer file `snapshot.commit`.
///
/// The sealed layer stays raw; compression, when enabled, happens once at
/// publish time under `[snapshot.publish_compression]`. Compressing here would
/// pay the cost on every pause and then pay it again at publish.
SandboxCaptureResult<std::string> RestackSnapshotOverlaybdRootfs(
    const SnapshotRestacker& restack, bool read_only,
    const std::string& live_runtime_image_config_path, const std::string& snapshot_root,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_OVERLAYBD_RESTACK_H_
