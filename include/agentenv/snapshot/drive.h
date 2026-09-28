// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/drive.rs
//
// Two views of one attached drive. `CommittedAttachedDrive` is what a
// published record stores — layer references, portable across nodes.
// `ResolvedAttachedDrive` is what the current node can boot — absolute local
// paths produced by repository resolution, which must never be written back
// into committed metadata.
#ifndef AGENTENV_SNAPSHOT_DRIVE_H_
#define AGENTENV_SNAPSHOT_DRIVE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/snapshot/layers.h"

namespace agentenv {
namespace snapshot {

/// Rust enum `ResolvedAttachedDrive`.
///
/// Rust has a single `Overlaybd` variant; `Kind` keeps that shape so a second
/// backing store would be a new enumerator rather than a reinterpretation of
/// these fields.
struct ResolvedAttachedDrive {
    enum class Kind { Overlaybd };

    Kind kind = Kind::Overlaybd;
    std::string drive_id;
    /// Node-local absolute path, valid only after resolution on this node.
    std::string image_config_path;
    bool read_only = false;
    /// Required on the wire: a drive of unknown size cannot be attached.
    uint64_t virtual_size = 0;
    /// Rust `#[serde(default)]`.
    std::string mount_path;
    /// Rust `#[serde(default, skip_serializing_if = "Option::is_none")]`.
    core::Optional<std::string> sub_path;

    /// Rust `ResolvedAttachedDrive::to_extra_drive`.
    sandbox::ExtraDrive ToExtraDrive() const;

    core::Json ToJson() const;
    static core::Expected<ResolvedAttachedDrive, std::string> FromJson(const core::Json& json);

    bool operator==(const ResolvedAttachedDrive& o) const;
    bool operator!=(const ResolvedAttachedDrive& o) const { return !(*this == o); }
};

/// Rust enum `CommittedAttachedDrive`.
struct CommittedAttachedDrive {
    enum class Kind { Overlaybd };

    Kind kind = Kind::Overlaybd;
    std::string drive_id;
    /// Layer references rather than a local path, so the record stays
    /// portable between nodes.
    std::vector<OverlaybdLayerRef> layers;
    bool read_only = false;
    uint64_t virtual_size = 0;
    /// Rust `#[serde(default)]`.
    std::string mount_path;
    /// Rust `#[serde(default, skip_serializing_if = "Option::is_none")]`.
    core::Optional<std::string> sub_path;

    core::Json ToJson() const;
    static core::Expected<CommittedAttachedDrive, std::string> FromJson(
        const core::Json& json);

    bool operator==(const CommittedAttachedDrive& o) const;
    bool operator!=(const CommittedAttachedDrive& o) const { return !(*this == o); }
};

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_DRIVE_H_
