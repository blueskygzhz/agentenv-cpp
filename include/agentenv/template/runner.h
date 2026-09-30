// SPDX-License-Identifier: MIT
// Rust: src/template/runner.rs — the in-sandbox half of a template build.
//
// The orchestration entry points (`build_template`,
// `build_template_from_snapshot`, `run_template_build`) construct a
// `FirecrackerSandbox` and drive start/capture/stop; they land with the
// Firecracker backend port. What lives here is everything that operates on an
// already-running sandbox through the `Executor` interface, which is where the
// behaviour that matters is:
//
//  * Default-user provisioning. envd resolves every filesystem operation
//    against the template's default user, and the e2b SDK bakes in the
//    assumption that the account exists (`from_dockerfile` injects
//    `USER user` when a Dockerfile sets none). Arbitrary OCI images do not
//    ship that account, so without this every SDK call against the published
//    template fails with envd's "invalid default user".
//
//  * The ready-check loop, which must treat a start command that exits
//    successfully as "done" rather than "dead", and must not let a start
//    command's failure be reported as a ready-command timeout.
#ifndef AGENTENV_TEMPLATE_RUNNER_H_
#define AGENTENV_TEMPLATE_RUNNER_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/process.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/errors.h"

namespace agentenv {
namespace tpl {

/// Rust `DEFAULT_READY_WITH_START_CMD` — what a start command with no ready
/// command gets. Matches e2b's own default so a template behaves the same on
/// both.
extern const char* const kDefaultReadyWithStartCmd;

/// Rust `READY_RETRY_INTERVAL`.
const int64_t kReadyRetryIntervalMs = 2 * 1000;
/// Rust `READY_TIMEOUT`.
const int64_t kReadyTimeoutMs = 10 * 60 * 1000;
/// Rust `PROVISION_TIMEOUT` — the probes can wait on NSS providers and
/// useradd/groupadd on passwd/group locks; an unbounded hang would wedge the
/// build worker.
const int64_t kProvisionTimeoutMs = 60 * 1000;
/// Rust `PROVISION_MISSING_TOOLING_EXIT` — the exit code the provisioning
/// script reserves for "this image ships no account-management tooling", which
/// is the one failure that stays a warning.
const int32_t kProvisionMissingToolingExit = 66;

/// Rust struct `ProvisionEntity` — one passwd/group entity a USER value names.
struct ProvisionEntity {
    /// "user" or "group", used in messages.
    std::string kind;
    std::string name;
    /// Command that succeeds when the entity already exists.
    std::string exists_probe;
    std::string create;
    std::string create_fallback;
};

/// Rust's `name.is_empty() || name == "root" || all-digits` skip plus the
/// plausible-name check.
///
/// Returns false when the name must be left alone: root always exists, a
/// numeric USER is resolved by envd without a passwd entry (Docker allows it),
/// and anything else — notably a value starting with `-` — would be misparsed
/// as options by useradd/groupadd.
bool ShouldProvisionEntityName(const std::string& name);

/// Rust's provisioning script.
///
/// Dispatches on which creation tool the image ships rather than chaining
/// `useradd || adduser`: a chain would mask a real failure of the present tool
/// behind the fallback's "command not found", and the resulting exit code
/// could not be told apart from "no tooling at all".
std::string BuildProvisionScript(const ProvisionEntity& entity);

/// Rust `ensure_entity`.
core::Expected<core::Unit, TemplateBuildFailure> EnsureEntity(sandbox::Executor* sandbox,
                                                               const ProvisionEntity& entity);

/// Rust `ensure_default_user`.
///
/// A `USER name:group` value names the account and the group independently
/// (Docker resolves the two separately and requires no membership), so a
/// missing named group is provisioned the same way as a missing account.
core::Expected<core::Unit, TemplateBuildFailure> EnsureDefaultUser(
    sandbox::Executor* sandbox, const snapshot::CommandContext& build_context);

/// Rust `prepare_startup`.
///
/// Returns nothing when both halves are blank, so a spec that says nothing
/// does not install an empty startup command. A start command with no ready
/// command gets the default one, because otherwise nothing would ever confirm
/// the template came up.
core::Optional<snapshot::StartupCommand> PrepareStartup(
    const core::Optional<snapshot::StartupCommand>& startup, bool override_startup,
    const snapshot::CommandContext& build_context);

/// Rust `ensure_start_command_still_running_or_success`.
///
/// Returns true when the start command already finished *successfully* (the
/// caller then stops tracking it), false when it is still running. A non-zero
/// exit is a build failure: the template's own start command did not work.
core::Expected<bool, TemplateBuildFailure> EnsureStartCommandStillRunningOrSuccess(
    sandbox::ProcessHandle* handle);

/// Injected clock, so the ready-loop's timeout behaviour is testable without
/// waiting ten minutes.
class RunnerClock {
 public:
    virtual ~RunnerClock() {}
    /// Monotonic milliseconds.
    virtual int64_t NowMs() = 0;
    virtual void SleepMs(int64_t duration_ms) = 0;
};

/// The real clock (`tokio::time::Instant` / `sleep_until` upstream).
RunnerClock* SystemRunnerClock();

/// Rust `run_ready_command`.
core::Expected<core::Unit, TemplateBuildFailure> RunReadyCommand(
    sandbox::Executor* sandbox, const snapshot::StartupCommand& startup,
    core::Optional<sandbox::ProcessHandle*>* start_cmd_handle, RunnerClock* clock);

/// Rust `run_startup_commands`.
core::Expected<core::Unit, TemplateBuildFailure> RunStartupCommands(
    sandbox::Executor* sandbox, const core::Optional<snapshot::StartupCommand>& startup,
    RunnerClock* clock);

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_RUNNER_H_
