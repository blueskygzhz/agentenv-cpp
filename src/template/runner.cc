// SPDX-License-Identifier: MIT
// Rust: src/template/runner.rs
#include "agentenv/template/runner.h"

#include <ctime>
#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/shell-util/shell.h"

namespace agentenv {
namespace tpl {

const char* const kDefaultReadyWithStartCmd = "/agentenv/bin/busybox sleep 20";

namespace {

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                    value[begin] == '\n' || value[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsBlank(const std::string& value) { return Trim(value).empty(); }

/// First whitespace-separated token — the binary name out of a command like
/// `useradd -m`, which is what `command -v` must be given.
std::string FirstToken(const std::string& command) {
    const std::size_t end = command.find_first_of(" \t");
    return end == std::string::npos ? command : command.substr(0, end);
}

std::string FormatMs(int64_t ms) {
    std::ostringstream out;
    out << (ms / 1000) << "s";
    return out.str();
}

class SystemClock : public RunnerClock {
 public:
    int64_t NowMs() override {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }
    void SleepMs(int64_t duration_ms) override {
        if (duration_ms <= 0) return;
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(duration_ms / 1000);
        ts.tv_nsec = static_cast<long>((duration_ms % 1000) * 1000000L);
        ::nanosleep(&ts, NULL);
    }
};

}  // namespace

RunnerClock* SystemRunnerClock() {
    static SystemClock clock;
    return &clock;
}

// ---------------------------------------------------------------------------
// Default-user provisioning
// ---------------------------------------------------------------------------

bool ShouldProvisionEntityName(const std::string& name) {
    // root always exists, and a numeric USER is resolved by envd without a
    // passwd entry (Docker allows a bare UID).
    if (name.empty() || name == "root") return false;
    bool all_digits = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') all_digits = false;
    }
    if (all_digits) return false;

    // Only names that are plausibly Unix accounts/groups. A value starting
    // with '-' would be misparsed as options by useradd/groupadd.
    const char first = name[0];
    const bool first_ok =
        (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        (first >= '0' && first <= '9') || first == '_';
    if (!first_ok) return false;
    for (std::size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string BuildProvisionScript(const ProvisionEntity& entity) {
    const std::string quoted       = shellutil::ShellQuote(entity.name);
    const std::string create_bin   = FirstToken(entity.create);
    const std::string fallback_bin = FirstToken(entity.create_fallback);

    std::ostringstream script;
    script << entity.exists_probe << " " << quoted << " >/dev/null 2>&1 && exit 0; "
           << "command -v " << create_bin << " >/dev/null 2>&1 && exec " << entity.create << " "
           << quoted << "; "
           << "command -v " << fallback_bin << " >/dev/null 2>&1 && exec "
           << entity.create_fallback << " " << quoted << "; "
           << "exit " << kProvisionMissingToolingExit;
    return script.str();
}

core::Expected<core::Unit, TemplateBuildFailure> EnsureEntity(sandbox::Executor* sandbox,
                                                              const ProvisionEntity& entity) {
    if (!ShouldProvisionEntityName(entity.name)) {
        if (!entity.name.empty() && entity.name != "root") {
            AGENTENV_WARN("template default " + entity.kind + " '" + entity.name +
                          "' is not a valid name; leaving it as-is");
        }
        return core::Unit();
    }

    const std::string script = BuildProvisionScript(entity);

    sandbox::ProcessOpts opts;
    opts.timeout_ms = core::Optional<int64_t>(kProvisionTimeoutMs);

    // /bin/sh, not bash: the images most likely to be missing the entry
    // (Alpine/BusyBox) are also the ones without bash, and the script is POSIX.
    std::vector<std::string> args;
    args.push_back("-c");
    args.push_back(script);

    const core::Expected<sandbox::ProcessOutput, std::string> output =
        sandbox->RunCommandWithOpts("/bin/sh", args, opts);

    if (!output.ok()) {
        // Executor errors are infrastructure failures: envd unreachable,
        // sandbox gone — those must fail the build too.
        return core::make_unexpected(TemplateBuildFailure::New(
            "provision template default " + entity.kind + ": " + output.error()));
    }
    if (output.value().exit_code == 0) return core::Unit();

    if (output.value().exit_code == kProvisionMissingToolingExit) {
        // An image with no account-management tooling built fine before this
        // provisioning existed, so it keeps building; only envd calls that
        // resolve the default user will fail at runtime.
        AGENTENV_WARN("image has neither " + FirstToken(entity.create) + " nor " +
                      FirstToken(entity.create_fallback) + "; template default " + entity.kind +
                      " left unprovisioned, and envd operations that resolve it will fail at "
                      "runtime");
        return core::Unit();
    }

    // A present tool that fails (permission denied, read-only filesystem,
    // corrupt passwd database) leaves the template with a default identity
    // known not to resolve — fail now rather than at every later SDK call.
    std::ostringstream message;
    message << "failed to provision template default " << entity.kind << " " << entity.name
            << ": exit status " << output.value().exit_code
            << CommandOutputSuffix(output.value().stdout_data, output.value().stderr_data);
    return core::make_unexpected(TemplateBuildFailure::New(message.str()));
}

core::Expected<core::Unit, TemplateBuildFailure> EnsureDefaultUser(
    sandbox::Executor* sandbox, const snapshot::CommandContext& build_context) {
    if (!build_context.user.has_value()) return core::Unit();
    const std::string& user = *build_context.user;

    // USER may be "name", "uid", "name:group", or "uid:gid".
    std::string account = user;
    core::Optional<std::string> group;
    const std::size_t colon = user.find(':');
    if (colon != std::string::npos) {
        account = Trim(user.substr(0, colon));
        group   = core::Optional<std::string>(Trim(user.substr(colon + 1)));
    } else {
        account = Trim(user);
    }

    // The group first: if a future change binds the account to it
    // (`useradd -g`), the group must already exist.
    if (group.has_value()) {
        ProvisionEntity entity;
        entity.kind            = "group";
        entity.name            = *group;
        entity.exists_probe    = "getent group";
        entity.create          = "groupadd";
        entity.create_fallback = "addgroup";
        const core::Expected<core::Unit, TemplateBuildFailure> ensured =
            EnsureEntity(sandbox, entity);
        if (!ensured.ok()) return core::make_unexpected(ensured.error());
    }

    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = account;
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";
    return EnsureEntity(sandbox, entity);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

core::Optional<snapshot::StartupCommand> PrepareStartup(
    const core::Optional<snapshot::StartupCommand>& startup, bool override_startup,
    const snapshot::CommandContext& build_context) {
    // Upstream also derives a start command from the image's ENTRYPOINT/CMD,
    // but that path is commented out: images requiring PID 1 (s6-overlay)
    // fail because the start command runs under `/bin/sh -c`, which holds
    // PID 1. Not reproduced here for the same reason.
    if (!startup.has_value()) return core::Optional<snapshot::StartupCommand>();

    snapshot::StartupCommand prepared = *startup;
    const bool start_cmd_empty = IsBlank(prepared.start_cmd);
    const bool ready_cmd_empty = IsBlank(prepared.ready_cmd);

    // Nothing to run and nothing to check: no startup at all, rather than an
    // empty command installed over the base image's.
    if (start_cmd_empty && ready_cmd_empty) {
        return core::Optional<snapshot::StartupCommand>();
    }
    if (!start_cmd_empty && ready_cmd_empty) {
        // Without this nothing would ever confirm the template came up.
        prepared.ready_cmd = kDefaultReadyWithStartCmd;
    }
    if (override_startup) {
        // The startup command inherits the environment the build steps
        // produced, so it runs in the same context a RUN step would have.
        prepared.context = build_context;
    }
    return core::Optional<snapshot::StartupCommand>(prepared);
}

core::Expected<bool, TemplateBuildFailure> EnsureStartCommandStillRunningOrSuccess(
    sandbox::ProcessHandle* handle) {
    const core::Expected<core::Optional<sandbox::ProcessOutput>, std::string> polled =
        handle->TryWait();
    if (!polled.ok()) {
        return core::make_unexpected(
            TemplateBuildFailure::New("start command failed while waiting for command"));
    }
    // Still running, which is the expected state for a start command.
    if (!polled.value().has_value()) return false;

    const sandbox::ProcessOutput& output = *polled.value();
    // A start command that exited cleanly is "done", not "dead": a one-shot
    // start command is legitimate.
    if (output.exit_code == 0) return true;

    std::ostringstream message;
    message << "start command failed: command exited with status " << output.exit_code
            << CommandOutputSuffix(output.stdout_data, output.stderr_data);
    return core::make_unexpected(TemplateBuildFailure::New(message.str()));
}

core::Expected<core::Unit, TemplateBuildFailure> RunReadyCommand(
    sandbox::Executor* sandbox, const snapshot::StartupCommand& startup,
    core::Optional<sandbox::ProcessHandle*>* start_cmd_handle, RunnerClock* clock) {
    const int64_t deadline = clock->NowMs() + kReadyTimeoutMs;
    uint64_t attempt = 0;

    std::string shell;
    std::string flag;
    startup.ShellCommand(&shell, &flag);

    for (;;) {
        const int64_t now = clock->NowMs();
        if (now >= deadline) {
            return core::make_unexpected(TemplateBuildFailure::New(
                "ready command timed out after " + FormatMs(kReadyTimeoutMs) + ": cmd='" +
                startup.ready_cmd + "'"));
        }

        sandbox::ProcessOpts opts;
        opts.envs = startup.context.env_vars;
        opts.cwd  = core::Optional<std::string>(startup.context.workdir);
        // Never outlive the overall deadline, so one hung attempt cannot push
        // the loop past its own timeout.
        opts.timeout_ms = core::Optional<int64_t>(deadline - now);
        ++attempt;

        // Checked every iteration: a start command that died must surface its
        // own failure rather than be reported as a ready-command timeout.
        if (start_cmd_handle->has_value()) {
            const core::Expected<bool, TemplateBuildFailure> still_ok =
                EnsureStartCommandStillRunningOrSuccess(**start_cmd_handle);
            if (!still_ok.ok()) return core::make_unexpected(still_ok.error());
            // Finished successfully, so stop tracking it.
            if (still_ok.value()) *start_cmd_handle = core::Optional<sandbox::ProcessHandle*>();
        }

        std::vector<std::string> args;
        args.push_back(flag);
        args.push_back(startup.ready_cmd);
        const core::Expected<sandbox::ProcessOutput, std::string> output =
            sandbox->RunCommandWithOpts(shell, args, opts);

        if (output.ok() && output.value().exit_code == 0) return core::Unit();

        // Both a non-zero exit and an executor error are retried until the
        // deadline: the sandbox may simply not be up yet.
        if (clock->NowMs() >= deadline) {
            std::ostringstream message;
            message << "ready command timed out after " << FormatMs(kReadyTimeoutMs)
                    << ": cmd='" << startup.ready_cmd << "'";
            if (output.ok()) {
                message << ", exit_code=" << output.value().exit_code
                        << CommandOutputSuffix(output.value().stdout_data,
                                               output.value().stderr_data);
            }
            return core::make_unexpected(TemplateBuildFailure::New(message.str()));
        }

        // Never sleep past the deadline.
        const int64_t remaining = deadline - clock->NowMs();
        clock->SleepMs(remaining < kReadyRetryIntervalMs ? remaining : kReadyRetryIntervalMs);
    }
}

core::Expected<core::Unit, TemplateBuildFailure> RunStartupCommands(
    sandbox::Executor* sandbox, const core::Optional<snapshot::StartupCommand>& startup,
    RunnerClock* clock) {
    if (!startup.has_value()) return core::Unit();

    std::unique_ptr<sandbox::ProcessHandle> owned_handle;
    core::Optional<sandbox::ProcessHandle*> start_handle;

    if (!IsBlank(startup->start_cmd)) {
        std::string shell;
        std::string flag;
        startup->ShellCommand(&shell, &flag);

        sandbox::ProcessOpts opts;
        opts.envs = startup->context.env_vars;
        opts.cwd  = core::Optional<std::string>(startup->context.workdir);

        std::vector<std::string> args;
        args.push_back(flag);
        args.push_back(startup->start_cmd);

        // Started, not run: the start command is expected to keep running
        // while the ready command polls it.
        core::Expected<std::unique_ptr<sandbox::ProcessHandle>, std::string> started =
            sandbox->StartProcess(shell, args, opts);
        if (!started.ok()) {
            return core::make_unexpected(TemplateBuildFailure::New(
                "start command failed: failed to execute '" + startup->start_cmd + "'"));
        }
        owned_handle = std::move(started.value());
        start_handle = core::Optional<sandbox::ProcessHandle*>(owned_handle.get());
    }

    if (!IsBlank(startup->ready_cmd)) {
        const core::Expected<core::Unit, TemplateBuildFailure> ready =
            RunReadyCommand(sandbox, *startup, &start_handle, clock);
        if (!ready.ok()) return core::make_unexpected(ready.error());
    }

    // A last look: the start command may have died between the ready check
    // succeeding and now, which still means the template does not work.
    if (start_handle.has_value()) {
        const core::Expected<bool, TemplateBuildFailure> still_ok =
            EnsureStartCommandStillRunningOrSuccess(*start_handle);
        if (!still_ok.ok()) return core::make_unexpected(still_ok.error());
    }

    return core::Unit();
}

}  // namespace tpl
}  // namespace agentenv
