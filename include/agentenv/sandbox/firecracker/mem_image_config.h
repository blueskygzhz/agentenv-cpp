// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs —
//   build_mem_snapshot_image_config, rewrite_lowers_with_runtime_roots,
//   local_layer_config, load_existing_image_config.
//
// The layer-stack bookkeeping for a memory snapshot. Each pause appends one
// new memory layer, so without this the stack would grow by one per pause
// until it hit overlaybd's 255-layer hard limit and the sandbox could never be
// resumed again.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_MEM_IMAGE_CONFIG_H_
#define AGENTENV_SANDBOX_FIRECRACKER_MEM_IMAGE_CONFIG_H_

#include <functional>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `DEFAULT_MAX_OVERLAYBD_SNAPSHOT_LAYERS`.
static const std::size_t kDefaultMaxOverlaybdSnapshotLayers = 32;

/// Rust `INHERITED_LAYERS_DIR`.
extern const char* const kInheritedLayersDir;

/// Rust `local_layer_config` — a lower that names a local file and carries no
/// descriptor.
///
/// `digest` and `size` are left empty/zero, as Rust's `..Default::default()`
/// does. They describe a *published* blob; a layer that only exists on this
/// node has neither, and inventing them would make a local layer look
/// fetchable from the repository.
storage::overlaybd::LayerConfig LocalLayerConfig(const std::string& path);

/// Rust `rewrite_lowers_with_runtime_roots`'s layer budget.
///
/// The full `kDefaultMaxOverlaybdSnapshotLayers` with an empty durable prefix,
/// one fewer per 4 prefix lowers, clamped to 1. Growing the prefix tightens
/// the budget, which keeps the total stack well below overlaybd's 255-layer
/// limit without compacting on every pause once the prefix is already large.
/// The clamp matters: at 0 a lone layer would be rewritten into itself on
/// every pause, forever.
std::size_t MaxRuntimeOwnedLayers(std::size_t durable_prefix_len);

/// Compacts a run of layers into `output_path`. Returns an empty optional when
/// there was nothing to compact, matching Rust's `Option<PathBuf>`.
typedef std::function<core::Expected<core::Optional<std::string>, std::string>(
    const std::vector<storage::overlaybd::LayerConfig>& layers, const std::string& output_path)>
    LayerCompactor;

/// Copies or hard-links a runtime layer into the snapshot directory.
/// Rust `link_or_copy_runtime_layer`.
typedef std::function<core::Expected<core::Unit, std::string>(
    const std::string& source, const std::string& destination)>
    LayerAdopter;

/// Rust `rewrite_lowers_with_runtime_roots`.
///
/// Splits the incoming stack into a durable prefix and a runtime-owned suffix,
/// then either compacts the suffix (plus the appended layer) into one new
/// layer, or adopts the suffix into the snapshot directory so the snapshot
/// stops depending on files the runtime may delete.
///
/// Two rules are load-bearing:
///
///   * **A suffix containing a remote lower cannot be compacted.** A lower with
///     an empty `file` lives in the repository, not on this node, so there is
///     nothing local to read; Rust bails rather than compacting a hole into the
///     output, which would silently produce a layer of zeros.
///   * **Adopted layers are renamed to `{index:04}/{basename}`.** The index
///     keeps stack order explicit in the directory, and two inherited layers
///     may legitimately share a basename.
core::Expected<std::vector<storage::overlaybd::LayerConfig>, std::string>
    RewriteLowersWithRuntimeRoots(
        const std::vector<storage::overlaybd::LayerConfig>& existing_lowers,
        const std::string& output_dir,
        const core::Optional<storage::overlaybd::LayerConfig>& appended_layer,
        const std::string& compaction_output_name,
        const std::vector<std::string>& runtime_owned_roots,
        const LayerCompactor& compact, const LayerAdopter& adopt);

/// Rust `build_mem_snapshot_image_config`.
///
/// `inherited` is the memory image config of the snapshot being resumed from,
/// or absent on a fresh launch (Rust's `load_existing_image_config` returns
/// `ImageConfig::default()` for `None`). `repo_blob_url` is carried over
/// verbatim: the inherited lowers may still reference repository blobs, and
/// dropping the URL would leave them unresolvable.
core::Expected<storage::overlaybd::ImageConfig, std::string> BuildMemSnapshotImageConfig(
    const core::Optional<storage::overlaybd::ImageConfig>& inherited,
    const std::string& new_layer_path, const std::string& output_dir,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_MEM_IMAGE_CONFIG_H_
