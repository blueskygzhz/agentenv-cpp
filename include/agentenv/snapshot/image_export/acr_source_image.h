// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/source_image.rs
//
// Reads an overlaybd `image.json` and decides, lower by lower, what a push to
// the source registry would have to do: reuse a blob the registry already
// holds, or upload a locally produced delta.
//
// Every rejection here is deliberate. A shape this planner does not model is
// refused rather than approximated, because the plan it produces is what the
// publisher turns into blob uploads and a manifest — a lower silently
// mis-classified would yield a manifest that names a blob nobody uploaded,
// and the image only fails to work later, on a different machine.
//
// `SourceRegistryRepository` itself lives in `acr.h`: it is shared with the
// client half, which needs it to aim a DELETE.
#ifndef AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_SOURCE_IMAGE_H_
#define AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_SOURCE_IMAGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/snapshot/image_export/acr.h"
#include "agentenv/snapshot/repository/errors.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

/// Rust `enum LocalSnapshotDeltaDescriptor`.
///
/// Two ways a local delta can be described, and the distinction decides how
/// the publisher reads the file:
///
/// * `Raw` — the bytes on disk *are* the blob, and its digest and size are
///   already known, so it can be streamed straight up.
/// * `DenseOverlaybd` — the file is a sparse overlaybd layer whose holes must
///   be materialised during the upload, so neither digest nor size is known
///   until that rewrite happens. Describing it here would hash the sparse
///   file and record a digest for bytes the registry will never receive.
struct LocalSnapshotDeltaDescriptor {
    enum class Kind { Raw, DenseOverlaybd };

    Kind kind = Kind::Raw;
    /// `Raw` only.
    std::string digest;
    /// `Raw` only.
    uint64_t size = 0;

    LocalSnapshotDeltaDescriptor() {}

    static LocalSnapshotDeltaDescriptor Raw(const std::string& digest, uint64_t size) {
        LocalSnapshotDeltaDescriptor out;
        out.kind = Kind::Raw;
        out.digest = digest;
        out.size = size;
        return out;
    }
    static LocalSnapshotDeltaDescriptor DenseOverlaybd() {
        LocalSnapshotDeltaDescriptor out;
        out.kind = Kind::DenseOverlaybd;
        return out;
    }

    bool is_raw() const { return kind == Kind::Raw; }
    bool is_dense_overlaybd() const { return kind == Kind::DenseOverlaybd; }

    /// Rust `#[derive(PartialEq, Eq)]`: the `DenseOverlaybd` variant carries
    /// nothing, so the stale `digest`/`size` of a reassigned value must not
    /// take part in the comparison.
    bool operator==(const LocalSnapshotDeltaDescriptor& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::DenseOverlaybd) return true;
        return digest == o.digest && size == o.size;
    }
    bool operator!=(const LocalSnapshotDeltaDescriptor& o) const { return !(*this == o); }
};

/// Rust `struct LocalSnapshotDelta`.
struct LocalSnapshotDelta {
    /// Canonicalised, so the publisher reads the same file the planner
    /// validated even if the config used a relative path or a symlink.
    std::string path;
    LocalSnapshotDeltaDescriptor descriptor;

    bool operator==(const LocalSnapshotDelta& o) const {
        return path == o.path && descriptor == o.descriptor;
    }
    bool operator!=(const LocalSnapshotDelta& o) const { return !(*this == o); }
};

/// Rust `enum SourceRegistryLayer`.
struct SourceRegistryLayer {
    enum class Kind { Remote, LocalDelta };

    Kind kind = Kind::Remote;
    /// `Remote` only.
    std::string digest;
    /// `Remote` only.
    uint64_t size = 0;
    /// `LocalDelta` only.
    LocalSnapshotDelta local_delta;

    SourceRegistryLayer() {}

    /// Rust `SourceRegistryLayer::Remote { digest, size }` — a blob the source
    /// registry already holds, so publishing only has to name it.
    static SourceRegistryLayer Remote(const std::string& digest, uint64_t size) {
        SourceRegistryLayer out;
        out.kind = Kind::Remote;
        out.digest = digest;
        out.size = size;
        return out;
    }
    /// Rust `SourceRegistryLayer::LocalDelta(delta)` — produced on this host
    /// and absent from the registry, so it has to be uploaded.
    static SourceRegistryLayer LocalDelta(const LocalSnapshotDelta& delta) {
        SourceRegistryLayer out;
        out.kind = Kind::LocalDelta;
        out.local_delta = delta;
        return out;
    }

    bool is_remote() const { return kind == Kind::Remote; }
    bool is_local_delta() const { return kind == Kind::LocalDelta; }

    bool operator==(const SourceRegistryLayer& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Remote) return digest == o.digest && size == o.size;
        return local_delta == o.local_delta;
    }
    bool operator!=(const SourceRegistryLayer& o) const { return !(*this == o); }
};

/// Rust `struct SourceRegistryImage`.
struct SourceRegistryImage {
    SourceRegistryRepository target;
    /// Base first, in `image.json` order. The order *is* the layer stack, so
    /// it is carried through unsorted.
    std::vector<SourceRegistryLayer> layers;

    bool operator==(const SourceRegistryImage& o) const {
        return target == o.target && layers == o.layers;
    }
    bool operator!=(const SourceRegistryImage& o) const { return !(*this == o); }
};

/// Rust `load_source_registry_image`.
///
/// Requires one image-level `repoBlobUrl` and at least one remote lower, and
/// requires every remote lower to come before every local delta.
///
/// The ordering rule is not tidiness: the lowers are the layer stack bottom-up,
/// and a remote lower appearing after a local delta would mean the snapshot's
/// own writes sit *below* a layer from the registry. Publishing that as a
/// linear manifest would silently reorder the stack, so it is refused.
///
/// `allow_loopback_http` is handed to `SourceRegistryRepository::Parse`; see
/// the note there.
repository::RepositoryResult<SourceRegistryImage> LoadSourceRegistryImage(
    const std::string& image_config_path, bool allow_loopback_http = false);

/// Rust `reject_unsupported_shape` — the per-lower shape check, exposed for
/// tests.
repository::RepositoryResult<core::Unit> RejectUnsupportedShape(
    std::size_t index, const storage::overlaybd::LayerConfig& layer);

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_IMAGE_EXPORT_ACR_SOURCE_IMAGE_H_
