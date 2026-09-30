// SPDX-License-Identifier: MIT
// Rust: src/template/runner.rs — the in-sandbox half: default-user
// provisioning, startup preparation, and the ready-check loop.
#include "agentenv/template/runner.h"

#include <map>
#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::tpl;  // NOLINT

namespace {

using agentenv::snapshot::CommandContext;
using agentenv::snapshot::StartupCommand;

/// One scripted reply for a command the runner issues.
struct ScriptedReply {
    int32_t     exit_code = 0;
    std::string stdout_data;
    std::string stderr_data;
    /// When set, the executor reports a transport failure instead.
    bool fail = false;
};

/// A handle whose exit state the test drives.
class ScriptedHandle : public sandbox::ProcessHandle {
 public:
    ScriptedHandle() {}

    int64_t Pid() const override { return 4242; }

    core::Expected<sandbox::ProcessOutput, std::string> Wait() override {
        if (!exited_) return core::make_unexpected(std::string("would block"));
        return output_;
    }

    core::Expected<core::Optional<sandbox::ProcessOutput>, std::string> TryWait() override {
        ++try_wait_calls_;
        if (fail_wait_) return core::make_unexpected(std::string("wait failed"));
        // Exits only once the test says so, and only after `exit_after_` polls,
        // which is how "still running while the ready loop polls" is modelled.
        if (!exited_ || try_wait_calls_ <= exit_after_) {
            return core::Optional<sandbox::ProcessOutput>();
        }
        return core::Optional<sandbox::ProcessOutput>(output_);
    }

    core::Expected<core::Unit, std::string> Kill(int) override { return core::Unit(); }

    void ExitWith(int32_t code, std::size_t after_polls = 0) {
        exited_           = true;
        output_.exit_code = code;
        exit_after_       = after_polls;
    }
    void FailWait() { fail_wait_ = true; }
    std::size_t try_wait_calls() const { return try_wait_calls_; }

 private:
    bool                   exited_    = false;
    bool                   fail_wait_ = false;
    std::size_t            exit_after_ = 0;
    std::size_t            try_wait_calls_ = 0;
    sandbox::ProcessOutput output_;
};

/// Records every command and replays scripted replies in order; the last
/// reply repeats once exhausted.
class ScriptedExecutor : public sandbox::Executor {
 public:
    struct Call {
        std::string                        cmd;
        std::vector<std::string>           args;
        core::Optional<std::string>        cwd;
        std::map<std::string, std::string> envs;
        core::Optional<int64_t>            timeout_ms;
    };

    core::Expected<sandbox::ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const sandbox::ProcessOpts& opts) override {
        Call call;
        call.cmd        = cmd;
        call.args       = args;
        call.cwd        = opts.cwd;
        call.envs       = opts.envs;
        call.timeout_ms = opts.timeout_ms;
        calls_.push_back(call);

        const ScriptedReply reply = NextReply();
        if (reply.fail) return core::make_unexpected(std::string("executor failed"));
        sandbox::ProcessOutput output;
        output.exit_code   = reply.exit_code;
        output.stdout_data = reply.stdout_data;
        output.stderr_data = reply.stderr_data;
        return output;
    }

    core::Expected<core::Unit, std::string> CreateDirAll(const std::string&) override {
        return core::Unit();
    }

    core::Expected<std::unique_ptr<sandbox::ProcessHandle>, std::string> StartProcess(
        const std::string& cmd, const std::vector<std::string>& args,
        const sandbox::ProcessOpts& opts) override {
        Call call;
        call.cmd  = cmd;
        call.args = args;
        call.cwd  = opts.cwd;
        call.envs = opts.envs;
        started_.push_back(call);
        if (fail_start_) return core::make_unexpected(std::string("spawn failed"));
        std::unique_ptr<sandbox::ProcessHandle> handle(new ScriptedHandle());
        return std::move(handle);
    }

    void Push(const ScriptedReply& reply) { replies_.push_back(reply); }
    void PushExit(int32_t code) {
        ScriptedReply reply;
        reply.exit_code = code;
        replies_.push_back(reply);
    }
    void PushFailure() {
        ScriptedReply reply;
        reply.fail = true;
        replies_.push_back(reply);
    }
    void FailStart() { fail_start_ = true; }

    const std::vector<Call>& calls() const { return calls_; }
    const std::vector<Call>& started() const { return started_; }

 private:
    ScriptedReply NextReply() {
        if (replies_.empty()) return ScriptedReply();
        const ScriptedReply reply = replies_[next_ < replies_.size() ? next_ : replies_.size() - 1];
        ++next_;
        return reply;
    }

    std::vector<ScriptedReply> replies_;
    std::size_t                next_ = 0;
    bool                       fail_start_ = false;
    std::vector<Call>          calls_;
    std::vector<Call>          started_;
};

/// A clock the test advances, so the ten-minute timeout is exercised instantly.
class FakeClock : public RunnerClock {
 public:
    int64_t NowMs() override { return now_ms_; }
    void SleepMs(int64_t duration_ms) override {
        now_ms_ += duration_ms;
        slept_.push_back(duration_ms);
    }
    void Advance(int64_t ms) { now_ms_ += ms; }
    const std::vector<int64_t>& slept() const { return slept_; }

 private:
    int64_t              now_ms_ = 0;
    std::vector<int64_t> slept_;
};

CommandContext ContextWithUser(const std::string& user) {
    CommandContext context;
    context.WithUser(core::Optional<std::string>(user));
    return context;
}

StartupCommand MakeStartup(const std::string& start_cmd, const std::string& ready_cmd) {
    StartupCommand startup;
    startup.start_cmd = start_cmd;
    startup.ready_cmd = ready_cmd;
    return startup;
}

}  // namespace

// ---- entity name filtering -------------------------------------------------

MT_TEST(runner_skips_names_that_must_not_be_provisioned) {
    // root always exists.
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("root"));
    MT_EXPECT_TRUE(!ShouldProvisionEntityName(""));
    // A numeric USER is resolved by envd without a passwd entry: Docker
    // allows a bare UID.
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("1000"));
    // Would be misparsed as options by useradd/groupadd.
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("-rf"));
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("bad name"));
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("semi;colon"));
    MT_EXPECT_TRUE(!ShouldProvisionEntityName("$(whoami)"));
}

MT_TEST(runner_accepts_plausible_unix_names) {
    MT_EXPECT_TRUE(ShouldProvisionEntityName("user"));
    MT_EXPECT_TRUE(ShouldProvisionEntityName("_svc"));
    MT_EXPECT_TRUE(ShouldProvisionEntityName("app-1"));
    MT_EXPECT_TRUE(ShouldProvisionEntityName("app.name"));
    // Leading digit is allowed as long as it is not all digits.
    MT_EXPECT_TRUE(ShouldProvisionEntityName("1user"));
}

// ---- provisioning script ---------------------------------------------------

MT_TEST(runner_provision_script_probes_then_dispatches_on_available_tool) {
    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "user";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    const std::string script = BuildProvisionScript(entity);
    // Already-exists short-circuits before any creation is attempted.
    MT_EXPECT_TRUE(script.find("id -u user >/dev/null 2>&1 && exit 0;") != std::string::npos);
    // `command -v` is given the *binary*, not the full command with its flags.
    MT_EXPECT_TRUE(script.find("command -v useradd >/dev/null 2>&1 && exec useradd -m user") !=
                   std::string::npos);
    MT_EXPECT_TRUE(script.find("command -v adduser >/dev/null 2>&1 && exec adduser -D user") !=
                   std::string::npos);
    // The reserved exit code distinguishes "no tooling" from a real failure.
    MT_EXPECT_TRUE(script.find("exit 66") != std::string::npos);
}

MT_TEST(runner_provision_script_passes_safe_names_through_unquoted) {
    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "app.name-1";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";
    // `shell_quote` leaves shell-safe names verbatim, so the script stays
    // readable; the safety comes from the name validator plus the quoting
    // below for anything unusual.
    MT_EXPECT_TRUE(BuildProvisionScript(entity).find("id -u app.name-1") != std::string::npos);
}

MT_TEST(runner_provision_script_quotes_a_name_needing_it) {
    ProvisionEntity entity;
    entity.kind            = "group";
    // `ShouldProvisionEntityName` would reject this, but the script builder
    // must not depend on that: quoting is what keeps a stray value from being
    // reinterpreted by the shell.
    entity.name            = "we ird";
    entity.exists_probe    = "getent group";
    entity.create          = "groupadd";
    entity.create_fallback = "addgroup";
    MT_EXPECT_TRUE(BuildProvisionScript(entity).find("'we ird'") != std::string::npos);
}

// ---- ensure_entity ---------------------------------------------------------

MT_TEST(runner_ensure_entity_succeeds_on_zero_exit) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);

    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "user";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    MT_EXPECT_TRUE(EnsureEntity(&sandbox, entity).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
    // /bin/sh, not bash: the images most likely to lack the account are also
    // the ones without bash.
    MT_EXPECT_EQ(sandbox.calls()[0].cmd, std::string("/bin/sh"));
    MT_EXPECT_EQ(sandbox.calls()[0].args[0], std::string("-c"));
    // Bounded, so a hung NSS lookup cannot wedge the build worker.
    MT_EXPECT_TRUE(sandbox.calls()[0].timeout_ms.has_value());
    MT_EXPECT_EQ(*sandbox.calls()[0].timeout_ms, kProvisionTimeoutMs);
}

MT_TEST(runner_ensure_entity_tolerates_an_image_without_tooling) {
    ScriptedExecutor sandbox;
    // An image with no useradd/adduser built fine before provisioning
    // existed, so it keeps building with a warning.
    sandbox.PushExit(kProvisionMissingToolingExit);

    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "user";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    MT_EXPECT_TRUE(EnsureEntity(&sandbox, entity).ok());
}

MT_TEST(runner_ensure_entity_fails_when_a_present_tool_fails) {
    ScriptedExecutor sandbox;
    ScriptedReply reply;
    // Not the reserved code: the tool exists and genuinely failed.
    reply.exit_code   = 1;
    reply.stderr_data = "useradd: Permission denied";
    sandbox.Push(reply);

    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "user";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    const core::Expected<core::Unit, TemplateBuildFailure> ensured =
        EnsureEntity(&sandbox, entity);
    MT_EXPECT_TRUE(!ensured.ok());
    // Failing now beats failing at every later SDK call against the template.
    MT_EXPECT_TRUE(ensured.error().reason.message.find("failed to provision") !=
                   std::string::npos);
    MT_EXPECT_TRUE(ensured.error().reason.message.find("exit status 1") != std::string::npos);
    // The tool's own diagnostic is carried through.
    MT_EXPECT_TRUE(ensured.error().reason.message.find("Permission denied") !=
                   std::string::npos);
}

MT_TEST(runner_ensure_entity_fails_on_an_executor_error) {
    ScriptedExecutor sandbox;
    sandbox.PushFailure();

    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "user";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    // envd unreachable / sandbox gone is infrastructure failure, not a
    // tolerable image quirk.
    MT_EXPECT_TRUE(!EnsureEntity(&sandbox, entity).ok());
}

MT_TEST(runner_ensure_entity_issues_no_command_for_a_skipped_name) {
    ScriptedExecutor sandbox;
    ProvisionEntity entity;
    entity.kind            = "user";
    entity.name            = "root";
    entity.exists_probe    = "id -u";
    entity.create          = "useradd -m";
    entity.create_fallback = "adduser -D";

    MT_EXPECT_TRUE(EnsureEntity(&sandbox, entity).ok());
    MT_EXPECT_TRUE(sandbox.calls().empty());
}

// ---- ensure_default_user ---------------------------------------------------

MT_TEST(runner_default_user_is_a_noop_without_a_user) {
    ScriptedExecutor sandbox;
    MT_EXPECT_TRUE(EnsureDefaultUser(&sandbox, CommandContext()).ok());
    MT_EXPECT_TRUE(sandbox.calls().empty());
}

MT_TEST(runner_default_user_provisions_the_account) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    MT_EXPECT_TRUE(EnsureDefaultUser(&sandbox, ContextWithUser("user")).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(sandbox.calls()[0].args[1].find("id -u user") != std::string::npos);
}

MT_TEST(runner_default_user_provisions_the_group_before_the_account) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    MT_EXPECT_TRUE(EnsureDefaultUser(&sandbox, ContextWithUser("app:staff")).ok());

    // The group goes first: if a future change binds the account to it
    // (`useradd -g`), the group must already exist.
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(sandbox.calls()[0].args[1].find("getent group staff") != std::string::npos);
    MT_EXPECT_TRUE(sandbox.calls()[0].args[1].find("groupadd") != std::string::npos);
    MT_EXPECT_TRUE(sandbox.calls()[1].args[1].find("id -u app") != std::string::npos);
}

MT_TEST(runner_default_user_trims_around_the_colon) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    MT_EXPECT_TRUE(EnsureDefaultUser(&sandbox, ContextWithUser(" app : staff ")).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(sandbox.calls()[0].args[1].find("getent group staff") != std::string::npos);
    MT_EXPECT_TRUE(sandbox.calls()[1].args[1].find("id -u app") != std::string::npos);
}

MT_TEST(runner_default_user_skips_a_numeric_uid_gid_pair) {
    ScriptedExecutor sandbox;
    // Docker allows a bare UID:GID with no passwd/group entry, and envd
    // resolves it during initialization.
    MT_EXPECT_TRUE(EnsureDefaultUser(&sandbox, ContextWithUser("1000:1000")).ok());
    MT_EXPECT_TRUE(sandbox.calls().empty());
}

MT_TEST(runner_default_user_stops_when_the_group_fails) {
    ScriptedExecutor sandbox;
    ScriptedReply reply;
    reply.exit_code = 1;
    sandbox.Push(reply);

    MT_EXPECT_TRUE(!EnsureDefaultUser(&sandbox, ContextWithUser("app:staff")).ok());
    // The account is not attempted once the group failed.
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
}

// ---- prepare_startup -------------------------------------------------------

MT_TEST(runner_prepare_startup_returns_nothing_for_an_absent_or_blank_command) {
    MT_EXPECT_TRUE(!PrepareStartup(core::Optional<StartupCommand>(), false, CommandContext())
                        .has_value());
    // Both halves blank means no startup at all, rather than an empty command
    // installed over the base image's.
    MT_EXPECT_TRUE(!PrepareStartup(core::Optional<StartupCommand>(MakeStartup("  ", "\t")), false,
                                   CommandContext())
                        .has_value());
}

MT_TEST(runner_prepare_startup_defaults_the_ready_command) {
    const core::Optional<StartupCommand> prepared = PrepareStartup(
        core::Optional<StartupCommand>(MakeStartup("/start.sh", "")), false, CommandContext());
    MT_EXPECT_TRUE(prepared.has_value());
    // Without this nothing would ever confirm the template came up.
    MT_EXPECT_EQ(prepared->ready_cmd, std::string(kDefaultReadyWithStartCmd));
}

MT_TEST(runner_prepare_startup_keeps_a_ready_only_command) {
    const core::Optional<StartupCommand> prepared =
        PrepareStartup(core::Optional<StartupCommand>(MakeStartup("", "test -f /ready")), false,
                       CommandContext());
    MT_EXPECT_TRUE(prepared.has_value());
    MT_EXPECT_EQ(prepared->start_cmd, std::string(""));
    // A ready-only startup is left alone: no start command to default for.
    MT_EXPECT_EQ(prepared->ready_cmd, std::string("test -f /ready"));
}

MT_TEST(runner_prepare_startup_adopts_the_build_context_only_when_overriding) {
    CommandContext built;
    built.WithEnvVar("FROM_BUILD", "1");
    built.WithWorkdir("/srv");

    // An override runs in the environment the build steps produced.
    const core::Optional<StartupCommand> overridden = PrepareStartup(
        core::Optional<StartupCommand>(MakeStartup("/start.sh", "true")), true, built);
    MT_EXPECT_EQ(overridden->context.workdir, std::string("/srv"));
    MT_EXPECT_TRUE(overridden->context.env_vars.find("FROM_BUILD") !=
                   overridden->context.env_vars.end());

    // An inherited startup keeps the context it was captured with.
    const core::Optional<StartupCommand> inherited = PrepareStartup(
        core::Optional<StartupCommand>(MakeStartup("/start.sh", "true")), false, built);
    MT_EXPECT_TRUE(inherited->context.env_vars.empty());
}

// ---- start-command liveness ------------------------------------------------

MT_TEST(runner_start_command_still_running_reports_false) {
    ScriptedHandle handle;
    const core::Expected<bool, TemplateBuildFailure> polled =
        EnsureStartCommandStillRunningOrSuccess(&handle);
    MT_EXPECT_TRUE(polled.ok());
    // Still running is the expected state for a start command.
    MT_EXPECT_TRUE(!polled.value());
}

MT_TEST(runner_start_command_clean_exit_reports_true) {
    ScriptedHandle handle;
    handle.ExitWith(0);
    const core::Expected<bool, TemplateBuildFailure> polled =
        EnsureStartCommandStillRunningOrSuccess(&handle);
    MT_EXPECT_TRUE(polled.ok());
    // A one-shot start command that succeeded is "done", not "dead".
    MT_EXPECT_TRUE(polled.value());
}

MT_TEST(runner_start_command_nonzero_exit_fails_the_build) {
    ScriptedHandle handle;
    handle.ExitWith(3);
    const core::Expected<bool, TemplateBuildFailure> polled =
        EnsureStartCommandStillRunningOrSuccess(&handle);
    MT_EXPECT_TRUE(!polled.ok());
    MT_EXPECT_TRUE(polled.error().reason.message.find("start command failed") !=
                   std::string::npos);
    MT_EXPECT_TRUE(polled.error().reason.message.find("status 3") != std::string::npos);
}

MT_TEST(runner_start_command_wait_error_fails_the_build) {
    ScriptedHandle handle;
    handle.FailWait();
    MT_EXPECT_TRUE(!EnsureStartCommandStillRunningOrSuccess(&handle).ok());
}

// ---- ready loop ------------------------------------------------------------

MT_TEST(runner_ready_command_succeeds_on_first_attempt) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
    // No sleeping when it passes immediately.
    MT_EXPECT_TRUE(clock.slept().empty());
}

MT_TEST(runner_ready_command_retries_until_it_passes) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(1);
    sandbox.PushExit(1);
    sandbox.PushExit(0);
    FakeClock clock;

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(clock.slept().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(clock.slept()[0], kReadyRetryIntervalMs);
}

MT_TEST(runner_ready_command_retries_through_executor_errors) {
    ScriptedExecutor sandbox;
    // The sandbox may simply not be up yet, so a transport error is retried
    // rather than failing the build immediately.
    sandbox.PushFailure();
    sandbox.PushExit(0);
    FakeClock clock;

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(2));
}

MT_TEST(runner_ready_command_times_out) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(1);  // repeats once exhausted
    FakeClock clock;

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    const core::Expected<core::Unit, TemplateBuildFailure> ready =
        RunReadyCommand(&sandbox, startup, &handle, &clock);
    MT_EXPECT_TRUE(!ready.ok());
    MT_EXPECT_TRUE(ready.error().reason.message.find("ready command timed out") !=
                   std::string::npos);
    // The last attempt's exit code is reported, so the operator sees why.
    MT_EXPECT_TRUE(ready.error().reason.message.find("exit_code=1") != std::string::npos);
    // The loop honoured the deadline rather than spinning forever.
    MT_EXPECT_TRUE(clock.NowMs() >= kReadyTimeoutMs);
}

MT_TEST(runner_ready_command_never_sleeps_past_the_deadline) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(1);
    FakeClock clock;
    // Start close enough to the deadline that a full retry interval would
    // overshoot it.
    clock.Advance(kReadyTimeoutMs - 500);

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(!RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    for (std::size_t i = 0; i < clock.slept().size(); ++i) {
        MT_EXPECT_TRUE(clock.slept()[i] <= kReadyRetryIntervalMs);
    }
}

MT_TEST(runner_ready_command_bounds_each_attempt_by_the_remaining_time) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    const StartupCommand startup = MakeStartup("", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    // One hung attempt must not be able to push the loop past its own
    // deadline.
    MT_EXPECT_TRUE(sandbox.calls()[0].timeout_ms.has_value());
    MT_EXPECT_TRUE(*sandbox.calls()[0].timeout_ms <= kReadyTimeoutMs);
}

MT_TEST(runner_ready_command_surfaces_a_dead_start_command) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(1);
    FakeClock clock;
    ScriptedHandle start;
    start.ExitWith(7);

    const StartupCommand startup = MakeStartup("/start.sh", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle(&start);
    const core::Expected<core::Unit, TemplateBuildFailure> ready =
        RunReadyCommand(&sandbox, startup, &handle, &clock);
    MT_EXPECT_TRUE(!ready.ok());
    // The start command's own failure, not a ready-command timeout — that is
    // what tells the operator what actually broke.
    MT_EXPECT_TRUE(ready.error().reason.message.find("start command failed") !=
                   std::string::npos);
    MT_EXPECT_TRUE(ready.error().reason.message.find("status 7") != std::string::npos);
}

MT_TEST(runner_ready_command_stops_tracking_a_finished_start_command) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(1);
    sandbox.PushExit(0);
    FakeClock clock;
    ScriptedHandle start;
    start.ExitWith(0);

    const StartupCommand startup = MakeStartup("/start.sh", "test -f /ready");
    core::Optional<sandbox::ProcessHandle*> handle(&start);
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    // Once it exited cleanly the loop stops polling it.
    MT_EXPECT_TRUE(!handle.has_value());
    MT_EXPECT_EQ(start.try_wait_calls(), static_cast<std::size_t>(1));
}

MT_TEST(runner_ready_command_passes_the_startup_context) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    StartupCommand startup = MakeStartup("", "test -f /ready");
    startup.context.WithEnvVar("READY_ENV", "1");
    startup.context.WithWorkdir("/srv");

    core::Optional<sandbox::ProcessHandle*> handle;
    MT_EXPECT_TRUE(RunReadyCommand(&sandbox, startup, &handle, &clock).ok());
    MT_EXPECT_EQ(*sandbox.calls()[0].cwd, std::string("/srv"));
    MT_EXPECT_EQ(sandbox.calls()[0].envs.find("READY_ENV")->second, std::string("1"));
}

// ---- run_startup_commands --------------------------------------------------

MT_TEST(runner_startup_commands_are_a_noop_when_absent) {
    ScriptedExecutor sandbox;
    FakeClock clock;
    MT_EXPECT_TRUE(
        RunStartupCommands(&sandbox, core::Optional<StartupCommand>(), &clock).ok());
    MT_EXPECT_TRUE(sandbox.calls().empty());
    MT_EXPECT_TRUE(sandbox.started().empty());
}

MT_TEST(runner_startup_starts_then_polls_ready) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    MT_EXPECT_TRUE(RunStartupCommands(
                       &sandbox,
                       core::Optional<StartupCommand>(MakeStartup("/start.sh", "test -f /ready")),
                       &clock)
                       .ok());
    // The start command is *started*, not run: it is expected to keep running
    // while the ready command polls it.
    MT_EXPECT_EQ(sandbox.started().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(sandbox.started()[0].args[1], std::string("/start.sh"));
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(sandbox.calls()[0].args[1], std::string("test -f /ready"));
}

MT_TEST(runner_startup_reports_a_spawn_failure) {
    ScriptedExecutor sandbox;
    sandbox.FailStart();
    FakeClock clock;

    const core::Expected<core::Unit, TemplateBuildFailure> ran = RunStartupCommands(
        &sandbox, core::Optional<StartupCommand>(MakeStartup("/start.sh", "true")), &clock);
    MT_EXPECT_TRUE(!ran.ok());
    MT_EXPECT_TRUE(ran.error().reason.message.find("failed to execute '/start.sh'") !=
                   std::string::npos);
    // The ready command is never attempted when the start command could not
    // even be spawned.
    MT_EXPECT_TRUE(sandbox.calls().empty());
}

MT_TEST(runner_startup_runs_ready_only_when_there_is_no_start_command) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    MT_EXPECT_TRUE(
        RunStartupCommands(&sandbox, core::Optional<StartupCommand>(MakeStartup("", "true")),
                           &clock)
            .ok());
    MT_EXPECT_TRUE(sandbox.started().empty());
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
}

MT_TEST(runner_startup_passes_the_context_to_the_start_command) {
    ScriptedExecutor sandbox;
    sandbox.PushExit(0);
    FakeClock clock;

    StartupCommand startup = MakeStartup("/start.sh", "true");
    startup.context.WithEnvVar("START_ENV", "1");
    startup.context.WithWorkdir("/srv");

    MT_EXPECT_TRUE(
        RunStartupCommands(&sandbox, core::Optional<StartupCommand>(startup), &clock).ok());
    MT_EXPECT_EQ(*sandbox.started()[0].cwd, std::string("/srv"));
    MT_EXPECT_EQ(sandbox.started()[0].envs.find("START_ENV")->second, std::string("1"));
}

int main() { return microtest::RunAll(); }
