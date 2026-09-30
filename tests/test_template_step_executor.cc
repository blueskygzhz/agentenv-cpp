// SPDX-License-Identifier: MIT
// Rust: src/template/step_executor.rs `mod tests`, plus coverage for the
// workdir normalizer's edge cases.
#include "agentenv/template/step_executor.h"

#include <map>
#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::tpl;  // NOLINT

namespace {

/// One recorded executor call: the operation, its arguments, and the cwd.
struct RecordedCall {
    std::string              op;
    std::vector<std::string> args;
    core::Optional<std::string> cwd;
};

/// Rust test double `RecordingSandbox` — records every command and directory
/// creation, and reports a configurable exit code.
class RecordingSandbox : public sandbox::Executor {
 public:
    explicit RecordingSandbox(int32_t exit_code) : exit_code_(exit_code) {}

    static RecordingSandbox Succeeding() { return RecordingSandbox(0); }
    static RecordingSandbox Failing() { return RecordingSandbox(1); }

    core::Expected<sandbox::ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const sandbox::ProcessOpts& opts) override {
        RecordedCall call;
        call.op   = cmd;
        call.args = args;
        call.cwd  = opts.cwd;
        calls_.push_back(call);
        last_envs_ = opts.envs;

        sandbox::ProcessOutput output;
        output.exit_code = exit_code_;
        return output;
    }

    core::Expected<core::Unit, std::string> CreateDirAll(const std::string& path) override {
        RecordedCall call;
        call.op = "create_dir_all";
        call.args.push_back(path);
        calls_.push_back(call);
        if (exit_code_ == 0) return core::Unit();
        return core::make_unexpected(std::string("permission denied"));
    }

    const std::vector<RecordedCall>& calls() const { return calls_; }
    const std::map<std::string, std::string>& last_envs() const { return last_envs_; }

 private:
    int32_t                            exit_code_;
    std::vector<RecordedCall>          calls_;
    std::map<std::string, std::string> last_envs_;
};

/// Rust test helper `run` — executes against a sandbox no step should touch.
snapshot::CommandContext Run(const std::vector<TemplateBuildStep>& steps) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, steps, snapshot::CommandContext());
    MT_EXPECT_TRUE(context.ok());
    return context.value();
}

std::vector<TemplateBuildStep> Steps1(const TemplateBuildStep& a) {
    std::vector<TemplateBuildStep> steps;
    steps.push_back(a);
    return steps;
}

std::vector<TemplateBuildStep> Steps2(const TemplateBuildStep& a, const TemplateBuildStep& b) {
    std::vector<TemplateBuildStep> steps;
    steps.push_back(a);
    steps.push_back(b);
    return steps;
}

}  // namespace

// ---- context-only steps ----------------------------------------------------

MT_TEST(step_user_sets_context_user) {
    const snapshot::CommandContext context = Run(Steps1(TemplateBuildStep::User("zzz")));
    MT_EXPECT_TRUE(context.user.has_value());
    MT_EXPECT_EQ(*context.user, std::string("zzz"));
}

MT_TEST(step_user_overrides_base_image_user) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    snapshot::CommandContext initial;
    initial.WithUser(core::Optional<std::string>(std::string("root")));

    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, Steps1(TemplateBuildStep::User("zzz")), initial);
    MT_EXPECT_TRUE(context.ok());
    MT_EXPECT_EQ(*context.value().user, std::string("zzz"));
}

MT_TEST(step_env_sets_env_var) {
    const snapshot::CommandContext context = Run(Steps1(TemplateBuildStep::Env("FOO", "bar")));
    MT_EXPECT_TRUE(context.env_vars.find("FOO") != context.env_vars.end());
    MT_EXPECT_EQ(context.env_vars.find("FOO")->second, std::string("bar"));
}

MT_TEST(step_env_later_value_replaces_the_earlier_one) {
    const snapshot::CommandContext context =
        Run(Steps2(TemplateBuildStep::Env("FOO", "first"), TemplateBuildStep::Env("FOO", "second")));
    MT_EXPECT_EQ(context.env_vars.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(context.env_vars.find("FOO")->second, std::string("second"));
}

MT_TEST(step_exposed_port_deduplicates) {
    std::vector<TemplateBuildStep> steps;
    steps.push_back(TemplateBuildStep::ExposedPort("8080"));
    steps.push_back(TemplateBuildStep::ExposedPort("8080"));
    steps.push_back(TemplateBuildStep::ExposedPort("443"));

    const snapshot::CommandContext context = Run(steps);
    MT_EXPECT_EQ(context.exposed_ports.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(context.exposed_ports[0], std::string("8080"));
    MT_EXPECT_EQ(context.exposed_ports[1], std::string("443"));
}

MT_TEST(step_exposed_port_from_base_image_is_not_duplicated) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    snapshot::CommandContext initial;
    std::vector<std::string> ports;
    ports.push_back("8080");
    initial.WithExposedPorts(ports);

    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, Steps1(TemplateBuildStep::ExposedPort("8080")),
                                       initial);
    MT_EXPECT_TRUE(context.ok());
    MT_EXPECT_EQ(context.value().exposed_ports.size(), static_cast<std::size_t>(1));
}

MT_TEST(step_volume_deduplicates) {
    std::vector<TemplateBuildStep> steps;
    steps.push_back(TemplateBuildStep::Volume("/data"));
    steps.push_back(TemplateBuildStep::Volume("/data"));
    steps.push_back(TemplateBuildStep::Volume("/logs"));

    const snapshot::CommandContext context = Run(steps);
    MT_EXPECT_EQ(context.volumes.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(context.volumes[0], std::string("/data"));
    MT_EXPECT_EQ(context.volumes[1], std::string("/logs"));
}

MT_TEST(step_label_replaces_a_repeated_key) {
    const snapshot::CommandContext context =
        Run(Steps2(TemplateBuildStep::Label("k", "v1"), TemplateBuildStep::Label("k", "v2")));
    MT_EXPECT_EQ(context.labels.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(context.labels.find("k")->second, std::string("v2"));
}

// ---- WORKDIR ---------------------------------------------------------------

MT_TEST(step_workdir_updates_workdir) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, Steps1(TemplateBuildStep::Workdir("/workspace")),
                                       snapshot::CommandContext());
    MT_EXPECT_TRUE(context.ok());
    MT_EXPECT_EQ(context.value().workdir, std::string("/workspace"));
}

MT_TEST(step_workdir_creates_the_directory) {
    // Docker's WORKDIR creates missing directories; images built from
    // Dockerfiles (and the e2b SDK's injected default workdir) rely on it.
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    MT_EXPECT_TRUE(TemplateStepExecutor()
                       .Execute(&sandbox, Steps1(TemplateBuildStep::Workdir("/app")),
                                snapshot::CommandContext())
                       .ok());

    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
    // Created through the filesystem service, never by exec'ing a binary from
    // the image — scratch/distroless images legitimately ship none.
    MT_EXPECT_EQ(sandbox.calls()[0].op, std::string("create_dir_all"));
    MT_EXPECT_EQ(sandbox.calls()[0].args.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(sandbox.calls()[0].args[0], std::string("/app"));
}

MT_TEST(step_workdir_resolves_relative_paths_against_current_workdir) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(
            &sandbox, Steps2(TemplateBuildStep::Workdir("/base"), TemplateBuildStep::Workdir("nested")),
            snapshot::CommandContext());
    MT_EXPECT_TRUE(context.ok());

    // Docker resolves a relative WORKDIR against the previous one; both the
    // created directory and the recorded workdir must be absolute.
    MT_EXPECT_EQ(context.value().workdir, std::string("/base/nested"));
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(sandbox.calls()[1].args[0], std::string("/base/nested"));
}

MT_TEST(step_workdir_normalizes_dot_components) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(
            &sandbox,
            Steps2(TemplateBuildStep::Workdir("/base/deep"), TemplateBuildStep::Workdir("../app")),
            snapshot::CommandContext());
    MT_EXPECT_TRUE(context.ok());

    // Docker records the normalized path, not /base/deep/../app.
    MT_EXPECT_EQ(context.value().workdir, std::string("/base/app"));
    MT_EXPECT_EQ(sandbox.calls()[1].args[0], std::string("/base/app"));
}

MT_TEST(step_workdir_failure_is_labelled_with_the_step) {
    RecordingSandbox sandbox = RecordingSandbox::Failing();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, Steps1(TemplateBuildStep::Workdir("/app")),
                                       snapshot::CommandContext());
    MT_EXPECT_TRUE(!context.ok());

    // The e2b SDKs parse reason.step as a number; the instruction text lives
    // in the message.
    MT_EXPECT_TRUE(context.error().reason.step.has_value());
    MT_EXPECT_EQ(*context.error().reason.step, std::string("1"));
    MT_EXPECT_TRUE(context.error().reason.message.find("WORKDIR /app") != std::string::npos);
}

// ---- RUN -------------------------------------------------------------------

MT_TEST(step_run_uses_a_login_shell_and_the_current_workdir) {
    RecordingSandbox sandbox = RecordingSandbox::Succeeding();
    std::vector<TemplateBuildStep> steps;
    steps.push_back(TemplateBuildStep::Env("FOO", "bar"));
    steps.push_back(TemplateBuildStep::Workdir("/srv"));
    steps.push_back(TemplateBuildStep::Run("make"));

    MT_EXPECT_TRUE(
        TemplateStepExecutor().Execute(&sandbox, steps, snapshot::CommandContext()).ok());

    // create_dir_all for the WORKDIR, then the RUN.
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(2));
    const RecordedCall& run = sandbox.calls()[1];
    MT_EXPECT_EQ(run.op, std::string("/bin/bash"));
    MT_EXPECT_EQ(run.args.size(), static_cast<std::size_t>(2));
    // `-lc` runs a login shell, so the image's profile scripts apply.
    MT_EXPECT_EQ(run.args[0], std::string("-lc"));
    MT_EXPECT_EQ(run.args[1], std::string("make"));
    // The RUN sees the workdir and environment the earlier steps established.
    MT_EXPECT_TRUE(run.cwd.has_value());
    MT_EXPECT_EQ(*run.cwd, std::string("/srv"));
    MT_EXPECT_EQ(sandbox.last_envs().find("FOO")->second, std::string("bar"));
}

MT_TEST(step_run_failure_reports_its_one_based_position) {
    RecordingSandbox sandbox = RecordingSandbox::Failing();
    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(
            &sandbox, Steps2(TemplateBuildStep::Env("A", "1"), TemplateBuildStep::Run("false")),
            snapshot::CommandContext());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_EQ(*context.error().reason.step, std::string("2"));
    MT_EXPECT_TRUE(context.error().reason.message.find("RUN false") != std::string::npos);
    MT_EXPECT_TRUE(context.error().reason.message.find("status 1") != std::string::npos);
}

MT_TEST(step_run_failure_reports_the_source_step_over_its_position) {
    RecordingSandbox sandbox = RecordingSandbox::Failing();
    // One client ENV step with two pairs arrives as two internal steps; the
    // failing RUN sits at internal position 3 but is client step 2.
    std::vector<TemplateBuildStep> steps;
    steps.push_back(TemplateBuildStep::Env("A", "1"));
    steps.push_back(TemplateBuildStep::Env("B", "2"));
    steps.push_back(TemplateBuildStep::Run("false"));
    steps[0].source_step = core::Optional<std::size_t>(static_cast<std::size_t>(1));
    steps[1].source_step = core::Optional<std::size_t>(static_cast<std::size_t>(1));
    steps[2].source_step = core::Optional<std::size_t>(static_cast<std::size_t>(2));

    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, steps, snapshot::CommandContext());
    MT_EXPECT_TRUE(!context.ok());
    // Reporting internal position 3 would point the SDK at the wrong entry in
    // its own stack trace.
    MT_EXPECT_EQ(*context.error().reason.step, std::string("2"));
}

MT_TEST(step_run_stops_at_the_first_failure) {
    RecordingSandbox sandbox = RecordingSandbox::Failing();
    std::vector<TemplateBuildStep> steps;
    steps.push_back(TemplateBuildStep::Run("false"));
    steps.push_back(TemplateBuildStep::Run("never-reached"));

    MT_EXPECT_TRUE(
        !TemplateStepExecutor().Execute(&sandbox, steps, snapshot::CommandContext()).ok());
    // Only the first RUN was attempted.
    MT_EXPECT_EQ(sandbox.calls().size(), static_cast<std::size_t>(1));
}

MT_TEST(step_empty_list_returns_the_initial_context_unchanged) {
    RecordingSandbox sandbox = RecordingSandbox::Failing();
    snapshot::CommandContext initial;
    initial.WithWorkdir("/base");
    initial.WithEnvVar("A", "1");

    const core::Expected<snapshot::CommandContext, TemplateBuildFailure> context =
        TemplateStepExecutor().Execute(&sandbox, std::vector<TemplateBuildStep>(), initial);
    MT_EXPECT_TRUE(context.ok());
    MT_EXPECT_EQ(context.value().workdir, std::string("/base"));
    MT_EXPECT_EQ(context.value().env_vars.find("A")->second, std::string("1"));
    MT_EXPECT_TRUE(sandbox.calls().empty());
}

// ---- ResolveWorkdir --------------------------------------------------------

MT_TEST(resolve_workdir_absolute_replaces_relative_joins) {
    MT_EXPECT_EQ(ResolveWorkdir("/base", "/other"), std::string("/other"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "nested"), std::string("/base/nested"));
    MT_EXPECT_EQ(ResolveWorkdir("/", "nested"), std::string("/nested"));
    // An empty current workdir is treated as the root.
    MT_EXPECT_EQ(ResolveWorkdir("", "nested"), std::string("/nested"));
}

MT_TEST(resolve_workdir_normalizes_dot_and_dotdot) {
    MT_EXPECT_EQ(ResolveWorkdir("/base/deep", "../app"), std::string("/base/app"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "./app"), std::string("/base/app"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "a/../b"), std::string("/base/b"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "."), std::string("/base"));
}

MT_TEST(resolve_workdir_cannot_escape_the_root) {
    // Popping past the root is a no-op, so no WORKDIR can name a path above /.
    MT_EXPECT_EQ(ResolveWorkdir("/", ".."), std::string("/"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "../../.."), std::string("/"));
    MT_EXPECT_EQ(ResolveWorkdir("/", "/../../etc"), std::string("/etc"));
}

MT_TEST(resolve_workdir_collapses_redundant_separators) {
    MT_EXPECT_EQ(ResolveWorkdir("/base", "a//b"), std::string("/base/a/b"));
    MT_EXPECT_EQ(ResolveWorkdir("/base/", "a"), std::string("/base/a"));
    MT_EXPECT_EQ(ResolveWorkdir("/base", "a/"), std::string("/base/a"));
}

int main() { return microtest::RunAll(); }
