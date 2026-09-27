// SPDX-License-Identifier: MIT
// Rust: src/volume.rs — the record/enum/error half.
//
// Split out of `volume.h` so that `snapshot/repository/interfaces.h` can name
// `VolumeRecord` (its catalog methods traffic in it) without pulling in
// `VolumeManager`, which itself depends on the repository interface. Rust has no
// such cycle because both live in one crate.
#ifndef AGENTENV_VOLUME_RECORD_H_
#define AGENTENV_VOLUME_RECORD_H_

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/snapshot/layers.h"

namespace agentenv {
namespace volume {

/// Rust `DEFAULT_VOLUME_SIZE_MB`.
const uint64_t kDefaultVolumeSizeMb = 64ULL * 1024ULL;
/// Rust `MAX_VOLUME_MOUNTS` — Firecracker exposes /dev/vdc through /dev/vdz
/// for extra drives, so 24 is a hardware-shaped ceiling, not a policy knob.
const std::size_t kMaxVolumeMounts = 24;
/// Rust `DEFAULT_VOLUME_PAGE_SIZE`.
const std::size_t kDefaultVolumePageSize = 100;
/// Rust `BYTES_PER_MB`.
const uint64_t kBytesPerMb = 1024ULL * 1024ULL;

/// Rust `is_valid_volume_component` — non-empty, <= 128 bytes, and only ASCII
/// alphanumerics plus `_` and `-`. Used for both names and ids, which is what
/// keeps a decoded pagination cursor from escaping the data directory.
bool IsValidVolumeComponent(const std::string& value);

/// Rust enum `VolumeMode`.
enum class VolumeMode {
    ReadOnly,
    Exclusive,  // Rust `#[default]`
};

/// Rust `impl Default for VolumeMode`.
inline VolumeMode VolumeModeDefault() { return VolumeMode::Exclusive; }
/// Serde spelling (`VolumeMode` is *not* `rename_all`d upstream).
const char* VolumeModeToString(VolumeMode mode);
core::Expected<VolumeMode, std::string> VolumeModeParse(const std::string& raw);

/// Rust enum `VolumeStatus` (`#[serde(rename_all = "lowercase")]`).
enum class VolumeStatus {
    Ready,  // Rust `#[default]`
    Uploading,
    Failed,
};

inline VolumeStatus VolumeStatusDefault() { return VolumeStatus::Ready; }
const char* VolumeStatusToString(VolumeStatus status);
core::Expected<VolumeStatus, std::string> VolumeStatusParse(const std::string& raw);

/// Rust struct `VolumeRecord` (`#[serde(rename_all = "camelCase")]`).
struct VolumeRecord {
    std::string id;
    std::string name;
    VolumeMode mode = VolumeMode::Exclusive;
    uint64_t size_mb = 0;
    VolumeStatus status = VolumeStatus::Ready;
    core::Optional<std::string> reserved_by_sandbox_id;
    /// Rust `#[serde(skip)]`: a node-local cache path, never persisted.
    core::Optional<std::string> backing_image_config;
    /// Repository-owned layers; `backing_image_config` is only a local cache.
    std::vector<snapshot::OverlaybdLayerRef> backing_layers;
    std::vector<std::string> read_only_mounts;
    /// Rust `pub(crate) deleting` with
    /// `#[serde(default, skip_serializing_if = "std::ops::Not::not")]`.
    bool deleting = false;

    /// Rust `VolumeRecord::mounted_by` — true for the exclusive owner or any
    /// read-only mount holder.
    bool MountedBy(const std::string& owner) const;

    /// Rust `VolumeRecord::replace_owner`. Returns whether anything changed.
    /// An unset `to` releases the lease.
    bool ReplaceOwner(const std::string& from, const core::Optional<std::string>& to);

    /// Rust `VolumeRecord::validate_catalog_update` — guards the durable
    /// catalog against illegal transitions. Returns the upstream error text.
    core::Expected<core::Unit, std::string> ValidateCatalogUpdate(const VolumeRecord& next) const;

    bool operator==(const VolumeRecord& other) const;
    bool operator!=(const VolumeRecord& other) const { return !(*this == other); }
};

/// Rust struct `VolumePage`.
struct VolumePage {
    std::vector<VolumeRecord> records;
    core::Optional<std::string> next_token;
};

/// Rust enum `VolumeError`.
enum class VolumeErrorKind {
    InvalidName,
    NameConflict,
    NotFound,
    Reserved,
    Uploading,
    Failed,
    MultipleSources,
    InvalidSize,
    SizeLimitExceeded,
    SizeMismatch,
    TooManyMountedVolumes,
    SourceNotFound,
    InvalidNextToken,
    InvalidPageLimit,
    Storage,
};

/// Rust `VolumeError` with its `#[error(...)]` messages.
struct VolumeError {
    VolumeErrorKind kind = VolumeErrorKind::Storage;
    /// Payload of the single-field variants (NameConflict / NotFound /
    /// Reserved / Uploading / Failed / SourceNotFound / Storage).
    std::string detail;
    /// Payload of `SizeLimitExceeded { max_size_mb }`.
    uint64_t max_size_mb = 0;
    /// Payload of `TooManyMountedVolumes { max_count }`.
    std::size_t max_count = 0;

    static VolumeError InvalidName();
    static VolumeError NameConflict(std::string name);
    static VolumeError NotFound(std::string lookup);
    static VolumeError Reserved(std::string owner);
    static VolumeError Uploading(std::string volume_id);
    static VolumeError Failed(std::string volume_id);
    static VolumeError MultipleSources();
    static VolumeError InvalidSize();
    static VolumeError SizeLimitExceeded(uint64_t max_size_mb);
    static VolumeError SizeMismatch();
    static VolumeError TooManyMountedVolumes(std::size_t max_count);
    static VolumeError SourceNotFound(std::string reference);
    static VolumeError InvalidNextToken();
    static VolumeError InvalidPageLimit();
    static VolumeError Storage(std::string message);

    /// Rust `thiserror` `Display`.
    std::string ToString() const;

    /// Rust `#[derive(Eq, PartialEq)]`, which the upstream tests rely on
    /// (`assert_eq!(err, VolumeError::SizeMismatch)`).
    bool operator==(const VolumeError& other) const;
    bool operator!=(const VolumeError& other) const { return !(*this == other); }
};

/// Rust `type Result<T> = Result<T, VolumeError>` at the volume boundary.
template <typename T>
using VolumeResult = core::Expected<T, VolumeError>;

// Stream operators, so assertion failures and logs print something readable
// instead of a raw integer. Found by ADL from within this namespace.
std::ostream& operator<<(std::ostream& os, VolumeMode mode);
std::ostream& operator<<(std::ostream& os, VolumeStatus status);
std::ostream& operator<<(std::ostream& os, VolumeErrorKind kind);
std::ostream& operator<<(std::ostream& os, const VolumeError& error);
std::ostream& operator<<(std::ostream& os, const VolumeRecord& record);

/// Rust `encode_volume_cursor` — base64url without padding.
std::string EncodeVolumeCursor(const std::string& volume_id);
/// Rust `decode_volume_cursor` — rejects bad base64, non-UTF8 and any id that
/// fails `is_valid_volume_component`, all as `InvalidNextToken`.
VolumeResult<std::string> DecodeVolumeCursor(const std::string& token);

/// Rust `validate_name`.
core::Expected<core::Unit, VolumeError> ValidateName(const std::string& name);
/// Rust `validate_volume_id` — a bad id is a `Storage` error, not `InvalidName`,
/// because it means the catalog itself is corrupt.
core::Expected<core::Unit, VolumeError> ValidateVolumeId(const std::string& id);

}  // namespace volume
}  // namespace agentenv
#endif  // AGENTENV_VOLUME_RECORD_H_
