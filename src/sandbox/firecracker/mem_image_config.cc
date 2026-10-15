// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs —
//   build_mem_snapshot_image_config, rewrite_lowers_with_runtime_roots,
//   local_layer_config.
#include "agentenv/sandbox/firecracker/mem_image_config.h"

#include <cstdio>

#include "agentenv/sandbox/firecracker/dirty_memory.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

const char* const kInheritedLayersDir = "inherited-layers";

namespace obd = storage::overlaybd;

obd::LayerConfig LocalLayerConfig(const std::string& path) {
    obd::LayerConfig layer;
    layer.file = path;
    // `digest` / `size` describe a published blob. A layer that exists only on
    // this node has neither, and filling them in would make it look fetchable
    // from the repository. Rust leaves them at `..Default::default()`.
    return layer;
}

std::size_t MaxRuntimeOwnedLayers(std::size_t durable_prefix_len) {
    const std::size_t shrink = durable_prefix_len / 4;
    // `saturating_sub` then `.max(1)`. The clamp is what stops a lone layer
    // being rewritten into itself on every pause, forever.
    const std::size_t budget = kDefaultMaxOverlaybdSnapshotLayers > shrink
                                   ? kDefaultMaxOverlaybdSnapshotLayers - shrink
                                   : 0;
    return budget > 1 ? budget : 1;
}

namespace {

/// Rust `Path::file_name().unwrap_or("runtime-layer.commit")`.
std::string BaseName(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    return name.empty() ? std::string("runtime-layer.commit") : name;
}

}  // namespace

core::Expected<std::vector<obd::LayerConfig>, std::string>
RewriteLowersWithRuntimeRoots(const std::vector<obd::LayerConfig>& existing_lowers,
                              const std::string& output_dir,
                              const core::Optional<obd::LayerConfig>& appended_layer,
                              const std::string& compaction_output_name,
                              const std::vector<std::string>& runtime_owned_roots,
                              const LayerCompactor& compact, const LayerAdopter& adopt) {
    // Split on the layers' file paths; `SplitRuntimeSuffix` partitions at the
    // *first* runtime-owned layer and treats everything above it as
    // runtime-owned too, because the stack is ordered and a durable layer
    // sitting on a runtime layer still depends on it.
    std::vector<std::string> files;
    for (std::size_t i = 0; i < existing_lowers.size(); ++i) {
        files.push_back(existing_lowers[i].file);
    }
    std::vector<std::string> durable_files;
    std::vector<std::string> runtime_files;
    SplitRuntimeSuffix(files, runtime_owned_roots, &durable_files, &runtime_files);

    const std::size_t prefix_len = durable_files.size();
    std::vector<obd::LayerConfig> lowers(existing_lowers.begin(),
                                         existing_lowers.begin() +
                                             static_cast<std::ptrdiff_t>(prefix_len));
    std::vector<obd::LayerConfig> runtime_owned(
        existing_lowers.begin() + static_cast<std::ptrdiff_t>(prefix_len),
        existing_lowers.end());

    const std::size_t max_runtime_owned = MaxRuntimeOwnedLayers(prefix_len);
    const std::size_t compactable =
        runtime_owned.size() + (appended_layer.has_value() ? 1u : 0u);

    if (compactable > max_runtime_owned) {
        std::vector<obd::LayerConfig> suffix = runtime_owned;
        if (appended_layer.has_value()) suffix.push_back(*appended_layer);

        // A lower with an empty `file` lives in the repository, not here, so
        // there is nothing local to read. Compacting anyway would write a hole
        // as zeros and produce a layer that reads as valid but empty.
        for (std::size_t i = 0; i < suffix.size(); ++i) {
            if (suffix[i].file.empty()) {
                return core::make_unexpected(std::string(
                    "cannot compact runtime-owned overlaybd suffix with remote lowers"));
            }
        }
        if (!compact) {
            return core::make_unexpected(std::string("no layer compactor configured"));
        }
        const core::Expected<core::Optional<std::string>, std::string> compacted =
            compact(suffix, output_dir + "/" + compaction_output_name);
        if (!compacted.ok()) return core::make_unexpected(compacted.error());
        // Rust pushes only when `Some`: nothing to compact leaves the prefix as
        // the whole stack rather than appending an empty layer.
        if (compacted.value().has_value()) {
            lowers.push_back(LocalLayerConfig(*compacted.value()));
        }
        return lowers;
    }

    // Under budget: adopt the runtime-owned suffix into the snapshot directory,
    // so the snapshot stops depending on files the runtime may delete.
    if (!runtime_owned.empty() && !adopt) {
        return core::make_unexpected(std::string("no layer adopter configured"));
    }
    const std::string inherited_dir = output_dir + "/" + kInheritedLayersDir;
    for (std::size_t i = 0; i < runtime_owned.size(); ++i) {
        // `{index:04}/{basename}`: the index keeps stack order explicit in the
        // directory, and two inherited layers may share a basename.
        char idx[8];
        std::snprintf(idx, sizeof(idx), "%04zu", i);
        const std::string destination =
            inherited_dir + "/" + idx + "/" + BaseName(runtime_owned[i].file);
        const core::Expected<core::Unit, std::string> moved =
            adopt(runtime_owned[i].file, destination);
        if (!moved.ok()) return core::make_unexpected(moved.error());
        runtime_owned[i].file = destination;
    }
    for (std::size_t i = 0; i < runtime_owned.size(); ++i) {
        lowers.push_back(runtime_owned[i]);
    }
    if (appended_layer.has_value()) lowers.push_back(*appended_layer);
    return lowers;
}

core::Expected<obd::ImageConfig, std::string> BuildMemSnapshotImageConfig(
    const core::Optional<obd::ImageConfig>& inherited, const std::string& new_layer_path,
    const std::string& output_dir, const std::vector<std::string>& runtime_owned_roots,
    const LayerCompactor& compact, const LayerAdopter& adopt) {
    // Rust `load_existing_image_config(None, ..)` yields `ImageConfig::default()`,
    // so a fresh launch starts from an empty stack rather than an error.
    const obd::ImageConfig base =
        inherited.has_value() ? *inherited : obd::ImageConfig();

    const core::Expected<std::vector<obd::LayerConfig>, std::string> lowers =
        RewriteLowersWithRuntimeRoots(
            base.lowers, output_dir,
            core::Optional<obd::LayerConfig>(LocalLayerConfig(new_layer_path)),
            "mem_compacted.commit", runtime_owned_roots, compact, adopt);
    if (!lowers.ok()) return core::make_unexpected(lowers.error());

    obd::ImageConfig out;
    // Only these two survive, as Rust's `..Default::default()` states. The
    // inherited lowers may still reference repository blobs, so dropping
    // `repo_blob_url` would leave them unresolvable; everything else describes
    // the *previous* device and must not be carried into the new snapshot.
    out.repo_blob_url = base.repo_blob_url;
    out.lowers = lowers.value();
    return out;
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
