// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/client.rs :: UblkDaemonClient
//
// The client half of the daemon protocol. Every ublk operation the sandbox
// path needs is one of these calls, so this is the ublk interface as far as
// the rest of the port is concerned.
//
// Three things here are not incidental:
//
//   * **One connection per call.** Rust's `call_inner` connects, sends,
//     receives and drops the stream. Holding a long-lived connection would
//     need reconnect handling on a socket whose peer can die, and the daemon
//     answers one request per connection anyway.
//   * **A dead daemon fails fast, before connecting.** Once the watchdog has
//     seen the process exit, further calls report that instead of waiting out
//     a connect timeout against a socket file that still exists.
//   * **An unknown outcome is not the same as a failure.** `restack_snapshot`
//     mutates live runtime state, so a timeout or a closed connection may
//     follow a *successful* seal. Only an explicit `Error` reply proves the
//     runtime is still safe to resume; everything else is terminal.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_CLIENT_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_CLIENT_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/ublk_daemon/protocol.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// Rust `DEFAULT_TIMEOUT`.
static const int64_t kDefaultTimeoutMs = 30 * 1000;
/// Rust `SNAPSHOT_TIMEOUT` — a restack seals and rewrites a layer, which is
/// bounded by disk throughput rather than by the daemon's responsiveness.
static const int64_t kSnapshotTimeoutMs = 360 * 1000;
/// Rust `READY_TIMEOUT`.
static const int64_t kReadyTimeoutMs = 30 * 1000;

/// Rust `RestackSnapshotTerminalFailure`.
///
/// A distinct error type, not a flag: the caller must not resume a runtime
/// whose restack outcome is unknown, and a plain error string would let that
/// distinction be dropped at the first `?`.
struct DaemonError {
    std::string message;
    /// True when the live runtime may already have been mutated.
    bool terminal = false;

    static DaemonError Recoverable(const std::string& m) {
        DaemonError e;
        e.message = m;
        return e;
    }
    static DaemonError Terminal(const std::string& m) {
        DaemonError e;
        e.message = m;
        e.terminal = true;
        return e;
    }
};

template <typename T>
using DaemonResult = core::Expected<T, DaemonError>;

/// Rust `RestackSnapshotStats`. All three fields are optional, and absent is
/// meaningful: an older daemon omits them.
struct RestackSnapshotStats {
    bool has_descriptor = false;
    core::Json descriptor;
    bool has_data_stat = false;
    core::Json data_stat;
    core::Optional<uint64_t> ext4_used_bytes;
};

/// Rust `OverlaybdRuntimeDevice`'s creation result.
struct CreatedRuntimeDevice {
    uint32_t dev_id = 0;
    std::string device_path;
    uint64_t actual_virtual_size = 0;
    std::string runtime_image_config_path;
};

/// Rust `UblkDevice` as returned by create/acquire.
struct CreatedDevice {
    uint32_t dev_id = 0;
    std::string device_path;
};

/// The transport. Separated from the client so the request/response matching
/// can be exercised against a fake daemon: the matching is where the error
/// classification lives, and it is not reachable through a real socket without
/// a daemon that can be made to misbehave.
class Transport {
 public:
    virtual ~Transport() {}

    /// Sends one request and returns the reply. Implementations must respect
    /// `timeout_ms` and report a timeout distinctly, because the caller's
    /// terminal/recoverable decision depends on telling "no answer" from
    /// "answered with an error".
    virtual core::Expected<core::Json, std::string> Call(const core::Json& request,
                                                          int64_t timeout_ms) = 0;
};

/// A real `AF_UNIX` transport: connect, send, receive, close.
///
/// `socket_path` is dialled fresh for every call, as Rust does.
std::shared_ptr<Transport> MakeUnixTransport(const std::string& socket_path);

/// Rust `UblkDaemonClient`.
class DaemonClient {
 public:
    /// `transport` carries the socket; `socket_path` is kept only so errors can
    /// name it, which is what makes a connect failure diagnosable.
    DaemonClient(std::shared_ptr<Transport> transport, std::string socket_path);

    const std::string& socket_path() const { return socket_path_; }

    /// Rust's watchdog result. Once set, every call fails immediately rather
    /// than dialling a socket file that outlives its process.
    void MarkDaemonDead(const std::string& reason);
    bool daemon_dead() const { return daemon_dead_; }

    /// Rust `shutdown` — marks the exit expected, then best-effort sends.
    ///
    /// The daemon may close the connection before replying, so a transport
    /// error here is not a failure: the shutdown was still requested.
    DaemonResult<core::Unit> Shutdown();

    /// Rust `create_overlaybd` — opens an existing image config as-is.
    DaemonResult<CreatedDevice> CreateOverlaybd(const std::string& image_config,
                                                const std::string& global_config);

    /// Rust `create_overlaybd_runtime_device` — the sandbox rootfs / extra
    /// drive path. The daemon materializes the runtime directory and returns
    /// the generated image config path.
    DaemonResult<CreatedRuntimeDevice> CreateOverlaybdRuntimeDevice(
        const std::string& source_image_config, const std::string& global_config,
        const std::string& runtime_dir, bool read_only,
        overlaybd::UpperMode runtime_upper_mode,
        const core::Optional<uint64_t>& requested_virtual_size,
        const core::Optional<uint64_t>& known_source_virtual_size, bool allow_shrink);

    /// Rust `acquire_overlaybd` — take a warm device from the pool.
    DaemonResult<CreatedDevice> AcquireOverlaybd(
        const std::string& image_config, const std::string& global_config,
        const core::Optional<uint64_t>& virtual_size, AccessMode access_mode);

    /// Rust `release_overlaybd` — return a device to the pool.
    DaemonResult<core::Unit> ReleaseOverlaybd(uint32_t dev_id);

    /// Rust `delete` — destroy a device. This is also the release path for
    /// dedicated memory devices, which must never go back to the warm pool.
    DaemonResult<core::Unit> Delete(uint32_t dev_id);

    /// Rust `update_size` — requires `UBLK_F_UPDATE_SIZE`.
    DaemonResult<core::Unit> UpdateSize(uint32_t dev_id, uint64_t new_sectors);

    /// Rust `get_features`.
    DaemonResult<uint64_t> GetFeatures();

    /// Rust `notify_sandbox_ready` — releases held background downloads.
    DaemonResult<core::Unit> NotifySandboxReady(const std::string& device_key);

    /// Rust `restack_snapshot`.
    ///
    /// Uses [`kSnapshotTimeoutMs`], and treats **any** non-`Error` outcome as
    /// terminal: a lost reply can follow a successful seal, so only an
    /// explicit `Error` establishes that the live runtime was left intact.
    DaemonResult<RestackSnapshotStats> RestackSnapshot(uint32_t dev_id,
                                                       const std::string& output_layer_path);

    /// Rust `start_pack_recording`.
    DaemonResult<core::Unit> StartPackRecording(uint32_t dev_id, const std::string& output,
                                                uint32_t max_pages, uint64_t min_window_ms,
                                                uint64_t quiet_ms, uint64_t max_window_ms);

    /// Rust `pack_recording_status`.
    DaemonResult<PackRecordingState> PackRecordingStatus(uint32_t dev_id);

    /// Rust `abort_pack_recording` — idempotent.
    DaemonResult<core::Unit> AbortPackRecording(uint32_t dev_id);

    /// Rust `prefetch_startup_pack`.
    ///
    /// Note the response matching: Rust accepts `Ok` and `Error` only, with no
    /// `TerminalError` arm, because the daemon answers `Ok` even when
    /// registration failed — prefetching is an optimisation and its failure
    /// must not fail a boot.
    DaemonResult<core::Unit> PrefetchStartupPack(const std::string& image_config,
                                                 const std::string& global_config,
                                                 const std::string& url, uint64_t pack_size,
                                                 const std::string& index_sha256,
                                                 uint64_t mem_virtual_size,
                                                 uint64_t timeout_secs);

 private:
    /// Rust `call` — the dead-daemon gate plus the timeout.
    core::Expected<DaemonResponse, DaemonError> Call(const DaemonRequest& request,
                                                     int64_t timeout_ms);

    std::shared_ptr<Transport> transport_;
    std::string socket_path_;
    bool daemon_dead_;
    std::string death_reason_;
    bool shutting_down_;
};

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_CLIENT_H_
