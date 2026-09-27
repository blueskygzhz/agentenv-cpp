// SPDX-License-Identifier: MIT
// Rust: src/privileges.rs
//
// Porting note. Rust has three entry points here:
//
//   * `require_runtime_capabilities` / `clear_ambient_capabilities` — pure
//     checks, ported as-is.
//   * `run_with_scoped_capabilities` — runs a closure on a short-lived thread
//     holding an exact capability set. Ported with std::thread, since Linux
//     capabilities and netns membership are *thread*-scoped, which is the
//     property the whole design rests on.
//   * `spawn_tokio_command_scoped` — the same trick for spawning a child
//     process. There is no Tokio here, so the C++ version returns the spawned
//     pid synchronously instead of a future; the capability handling is
//     identical.
#ifndef AGENTENV_PRIVILEGES_H_
#define AGENTENV_PRIVILEGES_H_

#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>

#include "agentenv/core/capability.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace privileges {

/// Rust `require_runtime_capabilities`.
///
/// Root is checked against the effective set; a non-root runtime is checked
/// against the delegable set (inheritable ∩ permitted ∩ effective), because
/// privileged helpers receive their one capability through the ambient set
/// immediately before `exec`.
core::Expected<core::Unit, std::string> RequireRuntimeCapabilities();

/// Rust `clear_ambient_capabilities` — keeps ordinary executables from
/// inheriting the server's network and namespace privileges.
core::Expected<core::Unit, std::string> ClearAmbientCapabilities();

/// Rust `run_with_scoped_capabilities`.
///
/// The operation runs on a fresh thread that holds exactly `capabilities` and
/// then exits, so the calling thread's privileges are never modified. The
/// operation reports failure by returning a non-empty message.
core::Expected<core::Unit, std::string> RunWithScopedCapabilities(
    const std::vector<int>& capabilities, const std::function<std::string()>& operation);

/// Describes a child process to launch under a scoped capability set.
struct ScopedSpawnRequest {
    /// argv[0] is resolved through PATH, as `Command::new` does.
    std::vector<std::string> argv;
    /// Rust `before_capability_scope`: runs on the launcher thread *before* its
    /// capabilities are replaced, which is what allows entering a network
    /// namespace (needs CAP_SYS_ADMIN) and then dropping to nothing. Returns a
    /// non-empty string to abort the spawn.
    std::function<std::string()> before_capability_scope;
    /// Capabilities the child should receive through the ambient set. Empty
    /// means the child gets none, which is the common case.
    std::vector<int> capabilities;
};

/// Rust `spawn_tokio_command_scoped`, minus the async wrapper: returns the pid
/// of the spawned child. The caller owns reaping it.
core::Expected<pid_t, std::string> SpawnScoped(const ScopedSpawnRequest& request);

}  // namespace privileges
}  // namespace agentenv
#endif  // AGENTENV_PRIVILEGES_H_
