// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/main.rs — the process-level setup that runs
// before and around the server: tracing, the descriptor limit, and the three
// ways this daemon is asked to stop.
#ifndef AGENTENV_STORAGE_UBLK_DAEMON_LIFECYCLE_H_
#define AGENTENV_STORAGE_UBLK_DAEMON_LIFECYCLE_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

/// Rust's `--log-level` value, parsed by clap into `tracing::LevelFilter`.
///
/// `off` has no counterpart in `core::LogLevel`, so it is carried separately
/// rather than folded into `Error`: a daemon told to be silent must not still
/// print errors.
struct LogLevelSpec {
    bool off;
    core::LogLevel level;

    LogLevelSpec() : off(false), level(core::LogLevel::Info) {}
};

/// Parses `off|error|warn|info|debug|trace`.
///
/// Rejects anything else instead of defaulting, because a typo that silently
/// became `info` would hide the `debug` someone was trying to turn on.
core::Expected<LogLevelSpec, std::string> ParseLogLevel(const std::string& text);

/// Rust `uvm_ublk::setup_tracing(log_file, log_level)`.
///
/// With a path, stderr is redirected onto it: this port's logger writes to
/// stderr, so redirecting it *is* "log to the file" and keeps anything written
/// by a child process in the same place. Without one, stderr is left alone.
///
/// The file is opened append-only. A daemon that is restarted must not erase
/// the log that explains why it stopped.
core::Expected<core::Unit, std::string> SetupTracing(
    const core::Optional<std::string>& log_file, const LogLevelSpec& level);

/// Rust's `setrlimit(Resource::RLIMIT_NOFILE, target, target)`.
///
/// Reports failure rather than returning it: Rust logs a warning and carries
/// on, because the daemon still works at a lower ceiling -- it simply manages
/// fewer devices before it starts failing.
static const uint64_t kTargetNoFileLimit = 1048576;
core::Expected<core::Unit, std::string> RaiseNoFileLimit(uint64_t target);

/// Rust `pidfd_open` (Linux 5.3+).
///
/// The descriptor becomes readable when the target exits, which is what lets
/// the daemon notice its parent is gone. Deliberately not
/// `prctl(PR_SET_PDEATHSIG)`: that fires when the *thread* that forked exits,
/// so a parent whose forking thread finishes would kill a daemon whose parent
/// is still alive.
core::Expected<int, std::string> PidfdOpen(int32_t pid);

/// Why `WaitForShutdown` returned.
enum ShutdownCause {
    kShutdownSigterm,
    kShutdownSigint,
    /// The parent process exited.
    kShutdownParentExited,
    /// The pidfd reported an error; treated as the parent being gone, since
    /// the alternative is a daemon nobody is supervising.
    kShutdownParentError
};

const char* ShutdownCauseName(ShutdownCause cause);

/// Installs SIGTERM/SIGINT handlers. Must run before `WaitForShutdown`, and
/// before any thread that should not receive them is started.
core::Expected<core::Unit, std::string> InstallSignalHandlers();

/// Blocks until SIGTERM, SIGINT, or the parent's exit.
///
/// Rust expresses this as a `tokio::select!` over three futures; with no
/// runtime here it is a poll over the pidfd plus the flag the signal handler
/// sets. Takes ownership of `parent_pidfd` and closes it.
core::Expected<ShutdownCause, std::string> WaitForShutdown(int parent_pidfd);

/// True once SIGTERM or SIGINT has been seen. For a server loop that needs to
/// check rather than block.
bool ShutdownRequested();

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_DAEMON_LIFECYCLE_H_
