// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/protocol.rs
//
// The daemon wire protocol. Every ublk operation the sandbox path needs —
// creating the rootfs block device, restacking a snapshot layer, releasing a
// device — is an RPC to `uvm-ublk-daemon` over a unix socket, not a syscall.
// So this protocol is the whole ublk interface as far as the rest of the port
// is concerned, and it is the part that can be aligned exactly: it is pure
// framing and JSON, with no kernel or io_uring dependency.
//
// Two shapes are reproduced rather than approximated:
//
//   * **Length prefix is big-endian.** Rust uses `len.to_be_bytes()`. A
//     little-endian prefix would be read as a huge length and rejected by the
//     size guard, so a byte-order mistake here looks like "message too large"
//     rather than like a byte-order mistake.
//   * **The tag field names differ between the two enums**: requests are
//     tagged `kind`, responses `status`, both `snake_case`. They are not
//     interchangeable, and a response decoded as a request would silently miss
//     every variant.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_PROTOCOL_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_PROTOCOL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// Rust `MAX_MESSAGE_SIZE` (16 MiB) — guards against a corrupt length prefix.
static const uint32_t kMaxMessageSize = 16u * 1024u * 1024u;

/// Rust `AccessMode`.
enum class AccessMode {
    /// Exclusive access (rootfs, snapshot resume).
    Exclusive,
    /// Shared access (memory snapshot, refcounted).
    Shared,
};
const char* AccessModeToString(AccessMode mode);
core::Expected<AccessMode, std::string> AccessModeParse(const std::string& raw);

// ── requests ───────────────────────────────────────────────────────────────

/// Rust `DaemonRequest` — a tagged union over `kind`.
struct DaemonRequest {
    enum class Kind {
        CreateOverlaybd,
        CreateOverlaybdRuntimeDevice,
        Delete,
        RestackSnapshot,
        GetFeatures,
        NotifySandboxReady,
        StartPackRecording,
        PackRecordingStatus,
        AbortPackRecording,
        PrefetchStartupPack,
        AcquireOverlaybd,
        ReleaseOverlaybd,
        UpdateSize,
        Shutdown,
    };

    Kind kind = Kind::GetFeatures;

    // CreateOverlaybd / CreateOverlaybdRuntimeDevice / AcquireOverlaybd /
    // PrefetchStartupPack
    std::string image_config;
    std::string global_config;

    // CreateOverlaybdRuntimeDevice
    std::string source_image_config;
    std::string runtime_dir;
    bool read_only = false;
    /// `#[serde(default = "default_runtime_upper_mode")]` — absent means
    /// `LogStructured`, not the enum's first value.
    overlaybd::UpperMode runtime_upper_mode = overlaybd::UpperMode::LogStructured;
    core::Optional<uint64_t> requested_virtual_size;
    core::Optional<uint64_t> known_source_virtual_size;
    /// `#[serde(default)]` — absent means false.
    bool allow_shrink = false;

    // Delete / RestackSnapshot / *PackRecording* / ReleaseOverlaybd / UpdateSize
    uint32_t dev_id = 0;

    // RestackSnapshot
    std::string output_layer_path;

    // NotifySandboxReady
    std::string device_key;

    // StartPackRecording
    std::string output;
    uint32_t max_pages = 0;
    uint64_t min_window_ms = 0;
    uint64_t quiet_ms = 0;
    uint64_t max_window_ms = 0;

    // PrefetchStartupPack
    std::string url;
    uint64_t pack_size = 0;
    std::string index_sha256;
    uint64_t mem_virtual_size = 0;
    uint64_t timeout_secs = 0;

    // AcquireOverlaybd
    core::Optional<uint64_t> virtual_size;
    AccessMode access_mode = AccessMode::Exclusive;

    // UpdateSize
    uint64_t new_sectors = 0;

    core::Json ToJson() const;
    static core::Expected<DaemonRequest, std::string> FromJson(const core::Json& value);
};

// ── responses ──────────────────────────────────────────────────────────────

/// Rust `PackRecordingState` — tagged over `state`.
struct PackRecordingState {
    enum class Kind {
        Recording,
        Done,
        Failed,
    };

    Kind kind = Kind::Recording;
    // Done
    uint32_t pages = 0;
    uint64_t bytes = 0;
    uint64_t remote_bytes = 0;
    std::string path;
    // Failed
    std::string reason;

    core::Json ToJson() const;
    static core::Expected<PackRecordingState, std::string> FromJson(const core::Json& value);
};

/// Rust `DaemonResponse` — a tagged union over `status`.
///
/// `TerminalError`, `InvalidRequest` and `Error` are distinct variants, not one
/// error with a category field. The caller treats them differently: a terminal
/// error means the daemon mutated state before failing, so a retry is not safe.
struct DaemonResponse {
    enum class Status {
        DeviceCreated,
        OverlaybdRuntimeDeviceCreated,
        DeviceAcquired,
        Released,
        SizeUpdated,
        Features,
        Deleted,
        RestackSnapshotCreated,
        PackRecording,
        Ok,
        TerminalError,
        InvalidRequest,
        Error,
    };

    Status status = Status::Ok;

    // DeviceCreated / OverlaybdRuntimeDeviceCreated / DeviceAcquired
    uint32_t dev_id = 0;
    std::string device_path;

    // OverlaybdRuntimeDeviceCreated
    uint64_t actual_virtual_size = 0;
    std::string runtime_image_config_path;

    // Features
    uint64_t flags = 0;

    // RestackSnapshotCreated — all three are `Option`, and absent is
    // meaningful: an older daemon omits them, which is not the same as a
    // daemon reporting zero.
    bool has_descriptor = false;
    core::Json descriptor;
    bool has_data_stat = false;
    core::Json data_stat;
    core::Optional<uint64_t> ext4_used_bytes;

    // PackRecording
    PackRecordingState state;

    // TerminalError / InvalidRequest / Error
    std::string message;

    /// True for the three error variants.
    bool IsError() const {
        return status == Status::TerminalError || status == Status::InvalidRequest ||
               status == Status::Error;
    }
    /// Rust's caller distinction: a terminal error must not be retried.
    bool IsTerminal() const { return status == Status::TerminalError; }

    core::Json ToJson() const;
    static core::Expected<DaemonResponse, std::string> FromJson(const core::Json& value);
};

// ── framing ────────────────────────────────────────────────────────────────

/// Rust `send_message` — 4-byte **big-endian** length prefix, then JSON.
std::vector<uint8_t> EncodeMessage(const core::Json& message);

/// Rust `recv_message`'s length check.
///
/// Returns the payload length, or an error when it exceeds
/// [`kMaxMessageSize`]. A wrong byte order fails here rather than later, which
/// is why the limit exists at all.
core::Expected<uint32_t, std::string> DecodeMessageLength(const uint8_t prefix[4]);

/// Decodes a complete frame (prefix + body). `consumed` receives the number of
/// bytes taken from `buf`.
///
/// Reports "incomplete" distinctly from a malformed message: a short read is
/// normal on a stream socket and must be retried, while a malformed one must
/// not be.
enum class DecodeStatus {
    Ok,
    Incomplete,
    Malformed,
};
DecodeStatus DecodeMessage(const uint8_t* buf, std::size_t len, core::Json* out,
                           std::size_t* consumed, std::string* err);

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_PROTOCOL_H_
