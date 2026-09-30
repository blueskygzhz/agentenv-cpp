// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{pool,startup_pack,config}.rs — the decisions
// around a Firecracker process's lifecycle, separated from the process
// management itself.
//
// The spawn/wait/signal machinery is still not ported; what is here is the set
// of rules that machinery consults, which is where the behaviour that can be
// got wrong lives.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_LIFECYCLE_H_
#define AGENTENV_SANDBOX_FIRECRACKER_LIFECYCLE_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `POOL_FIRECRACKER_STOP_TIMEOUT`.
const int64_t kPoolFirecrackerStopTimeoutMs = 2 * 1000;
/// Rust `POOL_PRIME_POLL_INTERVAL`.
const int64_t kPoolPrimePollIntervalMs = 20;

/// Rust `warm_stdio_paths`.
///
/// Both paths are present or both absent: a warm VM either has its output
/// captured to files or inherits the parent's stdio. Returning one of the two
/// would silently drop half the diagnostics.
void WarmStdioPaths(const std::string& work_dir, bool capture_output,
                    core::Optional<std::string>* stdout_path,
                    core::Optional<std::string>* stderr_path);

/// Rust `firecracker_pool_cleanup_result`.
///
/// Every failure is reported, joined with ` | `, rather than only the first.
/// Pool cleanup touches several independent entries and an operator needs to
/// see all of them to know what leaked.
core::Expected<core::Unit, std::string> FirecrackerPoolCleanupResult(
    const std::vector<std::string>& failures);

/// Rust `logging_enabled` — a blank log level means logging is off, so a
/// whitespace-only config value does not create an empty log file.
bool LoggingEnabled(const core::Optional<std::string>& log_level);

/// Rust `recording_enabled_for`.
///
/// Startup-pack recording is only worthwhile for the object-storage backend:
/// a POSIX-backed snapshot resolves its memory layers to plain repository file
/// paths, so there is no chain of small remote requests for the pack to
/// absorb. Both conditions must hold.
bool StartupPackRecordingEnabled(bool feature_enabled, bool repository_backend_is_oss);

/// Rust `create_firecracker_work_dir`.
///
/// With a parent, the parent is created first and the work directory goes
/// inside it; without one, the system temporary directory is used. The prefix
/// is `agentenv-fc-` either way, which is what identifies a leaked work
/// directory as ours.
core::Expected<std::string, std::string> CreateFirecrackerWorkDir(
    const core::Optional<std::string>& parent);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_LIFECYCLE_H_
