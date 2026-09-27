// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/snapshot.rs — `OverlaybdLayerRef`, `ManagedLayer`,
// `ExternalLayer`.
//
// Rust models the layer reference as an enum with payload:
//
//     pub enum OverlaybdLayerRef {
//         Managed(ManagedLayer),
//         External(ExternalLayer),
//     }
//
// C++11 has no `std::variant`, so this is a tagged struct carrying both
// payloads. Only the one selected by `kind` is meaningful; the constructors
// below are the only supported way to build a value, which keeps the invariant
// local to this header.
#ifndef AGENTENV_SNAPSHOT_LAYERS_H_
#define AGENTENV_SNAPSHOT_LAYERS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/optional.h"

namespace agentenv {
namespace snapshot {

/// Rust struct `ManagedLayer` — a layer whose bytes the repository owns.
struct ManagedLayer {
    std::string digest;
    uint64_t size = 0;
    /// Rust `#[serde(default, skip_serializing_if = "Option::is_none")]`.
    core::Optional<std::string> uuid;

    bool operator==(const ManagedLayer& other) const {
        if (digest != other.digest || size != other.size) return false;
        if (uuid.has_value() != other.uuid.has_value()) return false;
        return !uuid.has_value() || *uuid == *other.uuid;
    }
    bool operator!=(const ManagedLayer& other) const { return !(*this == other); }
};

/// Rust struct `ExternalLayer` — a layer that stays in a foreign registry.
struct ExternalLayer {
    std::string digest;
    std::string repo_blob_url;
    uint64_t size = 0;

    bool operator==(const ExternalLayer& other) const {
        return digest == other.digest && repo_blob_url == other.repo_blob_url &&
               size == other.size;
    }
    bool operator!=(const ExternalLayer& other) const { return !(*this == other); }
};

/// Rust enum `OverlaybdLayerRef`.
struct OverlaybdLayerRef {
    enum class Kind { Managed, External };

    Kind kind = Kind::Managed;
    ManagedLayer managed;
    ExternalLayer external;

    OverlaybdLayerRef() {}

    /// Rust `OverlaybdLayerRef::Managed(layer)`.
    static OverlaybdLayerRef Managed(const ManagedLayer& layer) {
        OverlaybdLayerRef out;
        out.kind = Kind::Managed;
        out.managed = layer;
        return out;
    }
    /// Rust `OverlaybdLayerRef::External(layer)`.
    static OverlaybdLayerRef External(const ExternalLayer& layer) {
        OverlaybdLayerRef out;
        out.kind = Kind::External;
        out.external = layer;
        return out;
    }

    bool is_managed() const { return kind == Kind::Managed; }
    bool is_external() const { return kind == Kind::External; }

    /// Digest of whichever variant is active.
    const std::string& digest() const {
        return kind == Kind::Managed ? managed.digest : external.digest;
    }
    /// Size of whichever variant is active.
    uint64_t size() const { return kind == Kind::Managed ? managed.size : external.size; }

    /// Rust `#[derive(PartialEq, Eq)]` on the enum: different variants never
    /// compare equal, and the active payload decides otherwise.
    bool operator==(const OverlaybdLayerRef& other) const {
        if (kind != other.kind) return false;
        return kind == Kind::Managed ? managed == other.managed : external == other.external;
    }
    bool operator!=(const OverlaybdLayerRef& other) const { return !(*this == other); }
};

/// Convenience for the `Vec<OverlaybdLayerRef>` comparisons `volume.rs` relies
/// on (`record.backing_layers == remote.backing_layers`).
bool LayerRefsEqual(const std::vector<OverlaybdLayerRef>& left,
                    const std::vector<OverlaybdLayerRef>& right);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_LAYERS_H_
