// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs — the capture half.
#include "agentenv/sandbox/firecracker/overlaybd_restack.h"

#include <cstdio>
#include <cstdlib>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

const char* const kManagedBaseLayerFile = "managed-base.commit";

namespace obd = storage::overlaybd;

namespace {

/// Rust `fs::canonicalize(p).unwrap_or_else(|_| p)` — the comparison form used
/// throughout this file. A path that cannot be resolved is compared verbatim:
/// it may simply not exist yet, and failing the whole snapshot over that would
/// be worse than comparing the spelling.
std::string CanonicalOrVerbatim(const std::string& path) {
    const core::Expected<std::string, std::string> resolved = core::fs::Canonicalize(path);
    return resolved.ok() ? resolved.value() : path;
}

/// Rust `prepare_specific_snapshot_layer_path` — drop a stale layer file.
///
/// A leftover from an earlier failed attempt would otherwise be handed to the
/// daemon as the seal destination, and the daemon appends.
core::Expected<core::Unit, std::string> PrepareSnapshotLayerPath(const std::string& path) {
    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(path);
    if (!removed.ok()) {
        return core::make_unexpected("remove stale overlaybd snapshot layer '" + path + "': " +
                                     removed.error());
    }
    return core::Unit();
}

/// Rust `rewrite_live_runtime_config_for_restack`.
///
/// Appends the sealed layer to the *live* config's lowers. This is what makes
/// the on-disk config agree with the device again: the daemon has reopened a
/// fresh empty upper above the sealed layer, so without this append the config
/// would describe a stack missing its newest lower, and a later resume would
/// lose every write the sealed layer holds.
core::Expected<core::Unit, std::string> RewriteLiveRuntimeConfigForRestack(
    const std::string& live_runtime_image_config_path, const std::string& snapshot_layer_path,
    const core::Optional<RestackedLayer>& descriptor) {
    core::Expected<obd::ImageConfig, std::string> loaded =
        obd::LoadImageConfig(live_runtime_image_config_path);
    if (!loaded.ok()) {
        return core::make_unexpected("load live overlaybd runtime config '" +
                                     live_runtime_image_config_path + "': " + loaded.error());
    }
    obd::ImageConfig image_config = loaded.value();
    image_config.lowers.push_back(
        LocalLayerConfigWithDescriptor(snapshot_layer_path, descriptor));

    return core::fs::WriteAtomically(live_runtime_image_config_path,
                                     obd::ImageConfigToJsonPretty(image_config),
                                     "live overlaybd runtime config");
}

/// Rust `enum LiveOverlaybdSnapshotState`.
///
/// The one bit of state the staging step's error classification depends on:
/// whether a seal happened. Modelled explicitly rather than as a bare
/// `Optional<string>` because `finish_staging` dispatches on it, and the two
/// cases differ in what a later failure *means*, not just in whether a path is
/// present.
struct LiveSnapshotState {
    bool restacked = false;
    std::string snapshot_layer_path;

    static LiveSnapshotState ReadOnly() { return LiveSnapshotState(); }
    static LiveSnapshotState Restacked(const std::string& path) {
        LiveSnapshotState s;
        s.restacked = true;
        s.snapshot_layer_path = path;
        return s;
    }

    core::Optional<std::string> LayerPath() const {
        if (!restacked) return core::Optional<std::string>();
        return core::Optional<std::string>(snapshot_layer_path);
    }

    /// Rust `finish_staging` — a staging failure after a seal is terminal,
    /// because the live runtime has already been mutated; the same failure on
    /// the read-only path left the sandbox untouched.
    SandboxCaptureResult<std::string> FinishStaging(
        const core::Expected<std::string, std::string>& staged) const {
        if (staged.ok()) return staged.value();
        if (restacked) {
            return core::make_unexpected(SandboxCaptureError::Terminal(staged.error()));
        }
        return core::make_unexpected(SandboxCaptureError::Recoverable(staged.error()));
    }
};

/// Rust `capture_live_overlaybd_snapshot`.
SandboxCaptureResult<LiveSnapshotState> CaptureLiveOverlaybdSnapshot(
    const SnapshotRestacker& restack, bool read_only,
    const std::string& live_runtime_image_config_path, const std::string& output_dir,
    const std::string& snapshot_layer_file_name) {
    if (read_only) {
        // Nothing writable to seal, so the live runtime is never touched and
        // the staged config is built straight from it.
        return LiveSnapshotState::ReadOnly();
    }

    const core::Expected<std::string, std::string> live_upper_data =
        RestackTargetUpperDataPath(live_runtime_image_config_path);
    if (!live_upper_data.ok()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "resolve restack source upper path: " + live_upper_data.error()));
    }

    const std::string snapshot_layer_path =
        core::fs::Join(output_dir, snapshot_layer_file_name);
    const core::Expected<core::Unit, std::string> prepared =
        PrepareSnapshotLayerPath(snapshot_layer_path);
    if (!prepared.ok()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(prepared.error()));
    }

    // The daemon can only seal into a file on the device's own filesystem, so
    // when the snapshot directory lives elsewhere the layer is produced beside
    // the live upper and copied across afterwards.
    const core::Expected<bool, std::string> same_fs =
        core::fs::OnSameFilesystem(live_upper_data.value(), output_dir);
    if (!same_fs.ok()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "validate restack snapshot filesystem precondition: " + same_fs.error()));
    }

    std::string live_snapshot_layer_path = snapshot_layer_path;
    if (!same_fs.value()) {
        const core::Optional<std::string> parent =
            core::fs::Parent(live_upper_data.value());
        if (!parent) {
            return core::make_unexpected(SandboxCaptureError::Recoverable(
                "restack source upper path has no parent directory: " +
                live_upper_data.value()));
        }
        live_snapshot_layer_path = core::fs::Join(*parent, snapshot_layer_file_name);
        const core::Expected<core::Unit, std::string> staged_prepared =
            PrepareSnapshotLayerPath(live_snapshot_layer_path);
        if (!staged_prepared.ok()) {
            return core::make_unexpected(SandboxCaptureError::Recoverable(
                "prepare same-filesystem restack snapshot layer '" +
                live_snapshot_layer_path + "': " + staged_prepared.error()));
        }
    }

    // Everything above this line is recoverable; everything below it runs
    // after the live upper has been sealed.
    const SandboxCaptureResult<core::Optional<RestackedLayer> > descriptor =
        restack(live_snapshot_layer_path);
    if (!descriptor.ok()) return core::make_unexpected(descriptor.error());

    if (live_snapshot_layer_path != snapshot_layer_path) {
        // Terminal on failure: the daemon has already sealed the old upper and
        // reopened a fresh one, so the live runtime must not be treated as
        // safely resumable just because the copy did not land.
        const core::Expected<core::Unit, std::string> copied = core::fs::CopyFileAtomically(
            live_snapshot_layer_path, snapshot_layer_path, "restack snapshot layer");
        if (!copied.ok()) {
            return core::make_unexpected(SandboxCaptureError::Terminal(
                "copy restack snapshot layer into managed snapshot dir: " + copied.error()));
        }
    }

    if (descriptor.value()) {
        const core::Expected<uint64_t, std::string> copied_size =
            core::fs::FileSize(snapshot_layer_path);
        if (!copied_size.ok()) {
            return core::make_unexpected(SandboxCaptureError::Terminal(
                "read restack snapshot layer metadata '" + snapshot_layer_path + "': " +
                copied_size.error()));
        }
        if (copied_size.value() != descriptor.value()->size) {
            // The sealed layer is deliberately kept: the live device may still
            // reference it, and deleting it would break the running sandbox on
            // top of the failed snapshot.
            char detail[160];
            std::snprintf(detail, sizeof(detail), ": descriptor says %llu, file has %llu",
                          static_cast<unsigned long long>(descriptor.value()->size),
                          static_cast<unsigned long long>(copied_size.value()));
            return core::make_unexpected(SandboxCaptureError::Terminal(
                "restack snapshot descriptor size mismatch for '" + snapshot_layer_path + "'" +
                detail));
        }
    }

    const core::Expected<core::Unit, std::string> rewritten =
        RewriteLiveRuntimeConfigForRestack(live_runtime_image_config_path, snapshot_layer_path,
                                           descriptor.value());
    if (!rewritten.ok()) {
        return core::make_unexpected(SandboxCaptureError::Terminal(
            "rewrite live runtime config after restack snapshot: " + rewritten.error()));
    }

    return LiveSnapshotState::Restacked(snapshot_layer_path);
}

}  // namespace

obd::LayerConfig LocalLayerConfigWithDescriptor(
    const std::string& path, const core::Optional<RestackedLayer>& descriptor) {
    obd::LayerConfig layer = LocalLayerConfig(path);
    if (descriptor) {
        layer.digest = descriptor->digest;
        layer.size   = descriptor->size;
    }
    return layer;
}

core::Expected<std::string, std::string> RestackTargetUpperDataPath(
    const std::string& live_runtime_image_config_path) {
    const core::Expected<obd::ImageConfig, std::string> loaded =
        obd::LoadImageConfig(live_runtime_image_config_path);
    if (!loaded.ok()) {
        return core::make_unexpected("load live overlaybd runtime config '" +
                                     live_runtime_image_config_path + "': " + loaded.error());
    }
    if (loaded.value().upper.data.empty()) {
        return core::make_unexpected(
            "restack snapshot requires writable upper.data in live runtime config '" +
            live_runtime_image_config_path + "'");
    }
    return loaded.value().upper.data;
}

std::vector<std::string> CanonicalizedRuntimeOwnedRoots(
    const std::string& managed_snapshot_base,
    const std::string& persisted_sandbox_store_path) {
    std::vector<std::string> roots;
    roots.push_back(CanonicalOrVerbatim(managed_snapshot_base));
    roots.push_back(CanonicalOrVerbatim(persisted_sandbox_store_path));
    return roots;
}

core::Expected<core::Unit, std::string> LinkOrCopyRuntimeLayer(const std::string& source,
                                                               const std::string& destination) {
    if (source == destination) return core::Unit();

    const core::Optional<std::string> parent = core::fs::Parent(destination);
    if (parent) {
        const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected("link or copy runtime overlaybd layer '" + source +
                                         "' -> '" + destination + "': " + made.error());
        }
    }
    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveFile(destination);
    if (!removed.ok()) {
        return core::make_unexpected("link or copy runtime overlaybd layer '" + source +
                                     "' -> '" + destination + "': " + removed.error());
    }

    const core::Expected<core::Unit, std::string> linked =
        core::fs::HardLink(source, destination);
    if (linked.ok()) return core::Unit();

    // Not an error: the two files may be on different filesystems, which is a
    // normal layout rather than a fault.
    AGENTENV_WARN("hard-link runtime overlaybd layer failed; falling back to sparse copy: "
                  << source << " -> " << destination << ": " << linked.error());

    // `--reflink=auto` keeps the copy cheap on a CoW filesystem, and
    // `--sparse=always` keeps an overlaybd layer's holes from being
    // materialized into real blocks.
    std::string command = "cp --reflink=auto --sparse=always '";
    command += source;
    command += "' '";
    command += destination;
    command += "'";
    const int status = std::system(command.c_str());
    if (status != 0) {
        core::fs::RemoveFile(destination);
        char detail[64];
        std::snprintf(detail, sizeof(detail), "%d", status);
        return core::make_unexpected("link or copy runtime overlaybd layer '" + source +
                                     "' -> '" + destination +
                                     "': sparse copy command failed with status " + detail);
    }
    return core::Unit();
}

core::Expected<std::string, std::string> StageOverlaybdSnapshotFromLiveRuntime(
    const std::string& live_runtime_image_config_path, const std::string& output_dir,
    const core::Optional<std::string>& snapshot_layer_path,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt) {
    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(output_dir);
    if (!made.ok()) {
        return core::make_unexpected("create overlaybd snapshot dir '" + output_dir + "': " +
                                     made.error());
    }

    const core::Expected<obd::ImageConfig, std::string> loaded =
        obd::LoadImageConfig(live_runtime_image_config_path);
    if (!loaded.ok()) {
        return core::make_unexpected("load overlaybd rootfs image config '" +
                                     live_runtime_image_config_path + "': " + loaded.error());
    }
    obd::ImageConfig image_config = loaded.value();

    core::Optional<obd::LayerConfig> appended_layer;
    if (snapshot_layer_path) {
        if (image_config.lowers.empty()) {
            return core::make_unexpected(std::string(
                "restack snapshot expected latest lower to be the newly claimed sealed upper "
                "layer"));
        }
        const std::string expected = CanonicalOrVerbatim(*snapshot_layer_path);
        const std::string actual = CanonicalOrVerbatim(image_config.lowers.back().file);
        if (actual != expected) {
            return core::make_unexpected("restack snapshot expected latest lower '" + expected +
                                         "' but found '" + actual + "'");
        }
        appended_layer = core::Optional<obd::LayerConfig>(image_config.lowers.back());
        image_config.lowers.pop_back();
    }

    const core::Expected<std::vector<obd::LayerConfig>, std::string> rewritten =
        RewriteLowersWithRuntimeRoots(image_config.lowers, output_dir, appended_layer,
                                      kManagedBaseLayerFile, runtime_owned_roots, compact,
                                      adopt);
    if (!rewritten.ok()) return core::make_unexpected(rewritten.error());

    image_config.lowers = rewritten.value();
    // A persisted snapshot is read-only. Keeping the live upper would make the
    // snapshot share a writable file with the running sandbox, so every resume
    // from it would see the source's later writes.
    image_config.upper = obd::UpperConfig();
    image_config.result_file = "./result.txt";

    const std::string output_path = core::fs::Join(output_dir, "image.json");
    const core::Expected<core::Unit, std::string> written = core::fs::WriteAtomically(
        output_path, obd::ImageConfigToJsonPretty(image_config),
        "overlaybd rootfs image config");
    if (!written.ok()) return core::make_unexpected(written.error());
    return output_path;
}

SandboxCaptureResult<std::string> RestackSnapshotOverlaybdDevice(
    const SnapshotRestacker& restack, bool read_only,
    const std::string& live_runtime_image_config_path, const std::string& output_dir,
    const std::string& snapshot_layer_file_name,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt) {
    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(output_dir);
    if (!made.ok()) {
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "create overlaybd snapshot dir '" + output_dir + "': " + made.error()));
    }

    const SandboxCaptureResult<LiveSnapshotState> live = CaptureLiveOverlaybdSnapshot(
        restack, read_only, live_runtime_image_config_path, output_dir,
        snapshot_layer_file_name);
    if (!live.ok()) return core::make_unexpected(live.error());

    const core::Expected<std::string, std::string> staged =
        StageOverlaybdSnapshotFromLiveRuntime(live_runtime_image_config_path, output_dir,
                                              live.value().LayerPath(), runtime_owned_roots,
                                              compact, adopt);
    return live.value().FinishStaging(staged);
}

SandboxCaptureResult<std::string> RestackSnapshotOverlaybdRootfs(
    const SnapshotRestacker& restack, bool read_only,
    const std::string& live_runtime_image_config_path, const std::string& snapshot_root,
    const std::vector<std::string>& runtime_owned_roots, const LayerCompactor& compact,
    const LayerAdopter& adopt) {
    return RestackSnapshotOverlaybdDevice(restack, read_only, live_runtime_image_config_path,
                                          core::fs::Join(snapshot_root, "rootfs"),
                                          "snapshot.commit", runtime_owned_roots, compact,
                                          adopt);
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
