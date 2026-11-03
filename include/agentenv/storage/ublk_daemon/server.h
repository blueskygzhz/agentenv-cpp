// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/server.rs — the socket lifecycle, the accept
// loop, and the per-connection request dispatch.
//
// Rust's `UblkDaemonServer` fuses two unrelated things. One is a unix-socket
// RPC server: clear a stale socket, bind, accept, read one framed request,
// dispatch it, write one framed reply, and undo a device handoff the client
// never received. The other is the device plane, which only exists on top of
// an io_uring control ring and real ublk char devices.
//
// Only the first is here. The second is behind [`DeviceHandler`], and that
// seam is what makes the first testable at all: every decision in this file --
// when a reply is written, when a failure becomes `TerminalError` rather than
// `Error`, when a just-created device has to be given back -- is reachable
// over a `socketpair` with a fake handler, and none of it is reachable through
// a real socket without a daemon that can be made to misbehave on demand.
//
// The ordering that matters, and why:
//
//   * **The reply to a device-creating request is a handoff.** It is the
//     client's only record of the device id, so a reply the client did not
//     read leaks a device nobody will ever release. The server therefore
//     checks delivery and rolls the device back when it can prove the client
//     did not get it.
//   * **"Could not prove delivery" is not the same as "not delivered."**
//     Releasing a device the client is already using corrupts a live sandbox,
//     while leaking one wastes a device id. Only a connection *reset* counts
//     as proof of non-delivery.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_SERVER_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_SERVER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/ublk_daemon/protocol.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// The three error flavours Rust separates at the connection boundary.
///
/// Rust gets them by downcasting a boxed `anyhow::Error` into
/// `RestackSnapshotTerminalFailure`, `runtime::InvalidRequest`, or neither.
/// There is no downcast here, so the flavour is carried explicitly. It cannot
/// be collapsed into one string: the client's retry decision reads it.
/// `kTerminal` means live state may already have been mutated and a retry is
/// unsafe; `kInvalidRequest` means the caller is at fault and a retry will
/// fail identically; `kInternal` means the daemon is at fault and a retry may
/// work.
struct HandlerError {
    enum Kind { kInternal, kInvalidRequest, kTerminal };

    Kind kind;
    std::string message;

    HandlerError() : kind(kInternal) {}

    static HandlerError Internal(const std::string& message);
    static HandlerError InvalidRequest(const std::string& message);
    static HandlerError Terminal(const std::string& message);
};

/// The device plane: everything in `server.rs` that needs the control ring.
///
/// One instance backs every connection, and connections are served on their
/// own threads, so implementations must be safe to call concurrently. Rust
/// says the same thing by sharing `Arc<DashMap<..>>` across spawned tasks.
class DeviceHandler {
 public:
    virtual ~DeviceHandler() {}

    /// Serves one request. Rust's `match request { .. }` arms.
    ///
    /// `DaemonRequest::Shutdown` never reaches here: Rust answers it in
    /// `handle_connection` by firing the shutdown notify, because stopping the
    /// accept loop is not a device operation.
    virtual core::Expected<DaemonResponse, HandlerError> Handle(
        const DaemonRequest& request) = 0;

    /// Whether a warm pool is configured. Rust reads `pool_state.is_some()`
    /// to pick the rollback path, and the two paths are not interchangeable:
    /// a pooled device must be *released* (returning one acquisition) while a
    /// raw one must be *deleted*.
    virtual bool HasPool() const = 0;

    /// Rust `stop_all_devices` — run once, after the accept loop exits.
    virtual void StopAllDevices() = 0;
};

/// The daemon-wide "stop accepting" flag.
///
/// Rust uses an `Arc<tokio::sync::Notify>` fired both by the in-band
/// `Shutdown` request and by the signal task. Here the accept loop polls with
/// a short timeout and re-checks this flag between polls -- the same shape
/// `WaitForShutdown` already uses in `lifecycle.cc`. That bounds the wakeup
/// latency instead of relying on a wakeup, so a flag set by a connection
/// thread microseconds before the loop blocks again cannot be lost.
class ShutdownFlag {
 public:
    ShutdownFlag() : requested_(false) {}

    void Request() { requested_.store(true); }
    bool requested() const { return requested_.load(); }

 private:
    ShutdownFlag(const ShutdownFlag&);
    ShutdownFlag& operator=(const ShutdownFlag&);

    std::atomic<bool> requested_;
};

/// Rust's stale-socket handling at the top of `run_with_ready_signal`.
///
/// A socket file that something answers on belongs to a live daemon, and
/// removing it would let two daemons manage devices behind each other's back;
/// that is refused. One nobody answers on is a leftover from a crash, and
/// removing it is the only way to come back up.
///
/// Exposed because the two cases look identical on disk and the distinction
/// is made by a connect attempt, which is exactly the part worth asserting.
core::Expected<core::Unit, std::string> ClearStaleSocket(const std::string& path);

/// Rust `handed_off_dev_id` — the device a response gives away, if any.
core::Optional<uint32_t> HandedOffDevId(const DaemonResponse& response);

/// Rust's `match response { Err(err) => .. }` arm, as a function.
DaemonResponse ResponseFromHandlerError(const HandlerError& error);

/// Rust `handle_connection` — one request, one reply, on an accepted socket.
///
/// `fd` is borrowed: the caller closes it. Returns an error only for the
/// cases Rust returns `Err` for -- a failed read, a failed send of a
/// non-handoff reply, or a rollback that did not complete. A connection that
/// closes before sending anything is not an error; it is how a client's
/// liveness probe looks.
core::Expected<core::Unit, std::string> ServeConnection(int fd, DeviceHandler* handler,
                                                        ShutdownFlag* shutdown);

/// Rust `UblkDaemonServer`, minus the device plane.
class UblkDaemonServer {
 public:
    /// `handler` is borrowed and must outlive the server, because connection
    /// threads call into it until `Run` returns.
    UblkDaemonServer(const std::string& socket_path, DeviceHandler* handler);

    /// Rust `run_with_ready_signal`.
    ///
    /// `ready` runs once the socket is bound *and* listening, before the first
    /// accept. That order is the whole contract with a supervisor: signalling
    /// earlier invites requests to a socket that does not accept yet, and
    /// signalling later makes a correct startup look like a hang.
    ///
    /// `parent_pidfd` is polled for readability alongside the listener, which
    /// folds Rust's separately spawned shutdown task into this one loop. It is
    /// consumed and closed. Pass -1 when there is no parent to watch.
    ///
    /// Returns once shutdown was requested (by signal, by the parent exiting,
    /// or by a `Shutdown` request) and every device has been stopped.
    core::Expected<core::Unit, std::string> Run(
        int parent_pidfd,
        const std::function<core::Expected<core::Unit, std::string>()>& ready);

    /// For the caller that wants to stop the loop from elsewhere.
    ShutdownFlag* shutdown() { return &shutdown_; }

 private:
    UblkDaemonServer(const UblkDaemonServer&);
    UblkDaemonServer& operator=(const UblkDaemonServer&);

    std::string socket_path_;
    DeviceHandler* handler_;
    ShutdownFlag shutdown_;
};

/// The accept loop's poll timeout. Short enough that a shutdown requested by
/// a connection thread is acted on promptly, long enough that an idle daemon
/// is not a busy loop. Matches `WaitForShutdown`'s 50 ms.
static const int kAcceptPollTimeoutMs = 50;

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_SERVER_H_
