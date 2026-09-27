// SPDX-License-Identifier: MIT
// Rust: src/template/build_spec.rs `mod tests` + src/template/errors.rs and the
// snapshot value types the spec depends on.
#include <string>
#include <vector>

#include "agentenv/template/build_spec.h"
#include "agentenv/template/errors.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::snapshot::CommandContext;
using agentenv::snapshot::SnapshotAlias;
using agentenv::snapshot::TemplateBuildErrorReason;
using agentenv::tpl::ImageConfigs;
using agentenv::tpl::TemplateBuildError;
using agentenv::tpl::TemplateBuildErrorKind;
using agentenv::tpl::TemplateBuildFailure;
using agentenv::tpl::TemplateBuildSpec;
using agentenv::tpl::TemplateBuildStep;
using agentenv::tpl::TemplateBuildStepKind;
using agentenv::tpl::TemplatePipelineError;

}  // namespace

// ---------------------------------------------------------------------------
// SnapshotAlias
// ---------------------------------------------------------------------------

MT_TEST(snapshot_alias_accepts_only_safe_characters) {
    MT_EXPECT_TRUE(SnapshotAlias::Parse("demo").ok());
    MT_EXPECT_TRUE(SnapshotAlias::Parse("my-template_01").ok());
    MT_EXPECT_EQ(SnapshotAlias::Parse("demo").value().ToString(), std::string("demo"));

    // Empty gets its own message; everything else reports the offending input.
    const agentenv::core::Expected<SnapshotAlias, std::string> empty = SnapshotAlias::Parse("");
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_EQ(empty.error(), std::string("snapshot alias cannot be empty"));

    const char* const invalid[] = {"has space", "has/slash", "has.dot", "dollar$", "unicode\xc3\xa9"};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const agentenv::core::Expected<SnapshotAlias, std::string> parsed =
            SnapshotAlias::Parse(invalid[i]);
        MT_EXPECT_TRUE(!parsed.ok());
        MT_EXPECT_TRUE(parsed.error().find("only supports ASCII") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// CommandContext
// ---------------------------------------------------------------------------

MT_TEST(command_context_defaults_to_root_workdir) {
    const CommandContext context;
    // Rust's Default is "/" rather than an empty string.
    MT_EXPECT_EQ(context.workdir, std::string("/"));
    MT_EXPECT_TRUE(context.env_vars.empty());
    MT_EXPECT_TRUE(!context.user.has_value());
    MT_EXPECT_TRUE(!context.entrypoint.has_value());
    MT_EXPECT_TRUE(!context.cmd.has_value());
}

MT_TEST(command_context_distinguishes_absent_from_empty_entrypoint) {
    CommandContext absent;
    CommandContext empty;
    empty.entrypoint = std::vector<std::string>();

    // An explicitly empty entrypoint overrides the base image's, so it must
    // not compare equal to an absent one.
    MT_EXPECT_TRUE(absent != empty);

    CommandContext also_empty;
    also_empty.entrypoint = std::vector<std::string>();
    MT_EXPECT_TRUE(empty == also_empty);
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

MT_TEST(command_output_suffix_prefers_stderr) {
    MT_EXPECT_EQ(agentenv::tpl::CommandOutputSuffix("out", "err"),
                 std::string("; stderr: err"));
    // Falls back to stdout only when stderr has nothing but whitespace.
    MT_EXPECT_EQ(agentenv::tpl::CommandOutputSuffix("out", "   \n\t "),
                 std::string("; stdout: out"));
    MT_EXPECT_EQ(agentenv::tpl::CommandOutputSuffix("  out  ", ""),
                 std::string("; stdout: out"));
    // Both blank produces an empty suffix so callers can concatenate freely.
    MT_EXPECT_EQ(agentenv::tpl::CommandOutputSuffix("", ""), std::string(""));
    MT_EXPECT_EQ(agentenv::tpl::CommandOutputSuffix(" \n", "\t"), std::string(""));
}

MT_TEST(template_build_error_messages_match_thiserror_text) {
    MT_EXPECT_EQ(TemplateBuildError::InvalidInput("no base image").ToString(),
                 std::string("invalid template build input: no base image"));
    MT_EXPECT_EQ(TemplateBuildError::System("vm would not boot").ToString(),
                 std::string("template build failed: vm would not boot"));
    // `with_source` does not change the rendered message; the source is a
    // separate chain link.
    const TemplateBuildError sourced =
        TemplateBuildError::WithSource("vm would not boot", "connection refused");
    MT_EXPECT_EQ(sourced.ToString(), std::string("template build failed: vm would not boot"));
    MT_EXPECT_TRUE(sourced.source.has_value());
    MT_EXPECT_EQ(*sourced.source, std::string("connection refused"));
}

MT_TEST(template_build_error_carries_the_failing_step) {
    const TemplateBuildFailure failure =
        TemplateBuildFailure::WithStep("apt-get failed", "RUN apt-get install nope");
    const TemplateBuildError error = TemplateBuildError::FromFailure(failure);

    MT_EXPECT_EQ(error.kind, TemplateBuildErrorKind::System);
    MT_EXPECT_EQ(error.ToString(), std::string("template build failed: apt-get failed"));
    MT_EXPECT_TRUE(error.step().has_value());
    MT_EXPECT_EQ(*error.step(), std::string("RUN apt-get install nope"));

    // A plain failure has no step attributed to it.
    MT_EXPECT_TRUE(!TemplateBuildError::FromFailure(TemplateBuildFailure::New("boom"))
                        .step()
                        .has_value());
}

MT_TEST(template_build_error_equality_accounts_for_step_and_source) {
    MT_EXPECT_TRUE(TemplateBuildError::System("a") == TemplateBuildError::System("a"));
    MT_EXPECT_TRUE(TemplateBuildError::System("a") != TemplateBuildError::System("b"));
    // Same message, different variant.
    MT_EXPECT_TRUE(TemplateBuildError::InvalidInput("a") != TemplateBuildError::System("a"));
    // Same message, one carries a source.
    MT_EXPECT_TRUE(TemplateBuildError::System("a") !=
                   TemplateBuildError::WithSource("a", "cause"));
    // Same message, one is attributed to a step.
    MT_EXPECT_TRUE(
        TemplateBuildError::FromFailure(TemplateBuildFailure::New("a")) !=
        TemplateBuildError::FromFailure(TemplateBuildFailure::WithStep("a", "RUN x")));
}

MT_TEST(pipeline_error_is_transparent_over_its_inner_error) {
    const TemplatePipelineError build =
        TemplatePipelineError::FromBuild(TemplateBuildError::System("inner"));
    // `#[error(transparent)]`: no wrapper prefix of its own.
    MT_EXPECT_EQ(build.ToString(), std::string("template build failed: inner"));

    const agentenv::snapshot::repository::RepositoryError repository_error =
        agentenv::snapshot::repository::RepositoryError::Unsupported("volume catalog");
    const TemplatePipelineError repository =
        TemplatePipelineError::FromRepository(repository_error);
    MT_EXPECT_EQ(repository.ToString(), repository_error.ToString());
}

MT_TEST(with_reason_source_keeps_both_step_and_cause) {
    const TemplateBuildError error = TemplateBuildError::WithReasonSource(
        TemplateBuildErrorReason::WithStep("copy failed", "COPY . /app"), "disk full");

    MT_EXPECT_EQ(error.ToString(), std::string("template build failed: copy failed"));
    MT_EXPECT_EQ(*error.step(), std::string("COPY . /app"));
    MT_EXPECT_EQ(*error.source, std::string("disk full"));
}

// ---------------------------------------------------------------------------
// TemplateBuildSpec
// ---------------------------------------------------------------------------

MT_TEST(getters_preserve_build_description) {
    // Rust: `getters_preserve_build_description`.
    TemplateBuildSpec spec;
    spec.FromExistingRootfs("/tmp/rootfs.ext4")
        .Alias("demo")
        .Resources(2, 256)
        .StartCmd("echo start")
        .ReadyCmd("echo ready")
        .Run("echo hi");

    MT_EXPECT_TRUE(spec.RootfsBase().has_value());
    MT_EXPECT_TRUE(spec.RootfsBase()->is_ext4());
    MT_EXPECT_EQ(spec.RootfsBase()->image_path, std::string("/tmp/rootfs.ext4"));

    const agentenv::tpl::TemplateBuildResult<Optional<SnapshotAlias> > alias =
        spec.ParsedAlias();
    MT_EXPECT_TRUE(alias.ok());
    MT_EXPECT_TRUE(alias.value().has_value());
    MT_EXPECT_EQ(alias.value()->ToString(), std::string("demo"));

    MT_EXPECT_TRUE(spec.ResourcesRef().has_value());
    MT_EXPECT_EQ(spec.ResourcesRef()->cpu_count, static_cast<uint32_t>(2));
    MT_EXPECT_EQ(spec.ResourcesRef()->memory_mib, static_cast<uint32_t>(256));
    // Disk size is only known after the build.
    MT_EXPECT_EQ(spec.ResourcesRef()->disk_size_mib, static_cast<uint32_t>(0));

    MT_EXPECT_EQ(spec.Steps().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(*spec.StartCmdRef(), std::string("echo start"));
    MT_EXPECT_EQ(*spec.ReadyCmdRef(), std::string("echo ready"));
}

MT_TEST(empty_startup_commands_are_ignored) {
    // Rust: `empty_startup_commands_are_ignored`.
    TemplateBuildSpec spec;
    spec.StartCmd("  ").ReadyCmd("");

    MT_EXPECT_TRUE(!spec.StartCmdRef().has_value());
    MT_EXPECT_TRUE(!spec.ReadyCmdRef().has_value());
    MT_EXPECT_TRUE(!spec.OverridesStartup());
}

MT_TEST(startup_commands_are_stored_untrimmed_but_clearable) {
    TemplateBuildSpec spec;
    // A non-blank command keeps its original spacing.
    spec.StartCmd("  echo start  ");
    MT_EXPECT_EQ(*spec.StartCmdRef(), std::string("  echo start  "));
    MT_EXPECT_TRUE(spec.OverridesStartup());

    // A later blank command clears the override.
    spec.StartCmd("\t\n");
    MT_EXPECT_TRUE(!spec.StartCmdRef().has_value());
    MT_EXPECT_TRUE(!spec.OverridesStartup());
}

MT_TEST(overrides_startup_reacts_to_either_command) {
    TemplateBuildSpec only_ready;
    only_ready.ReadyCmd("echo ready");
    MT_EXPECT_TRUE(only_ready.OverridesStartup());

    TemplateBuildSpec neither;
    MT_EXPECT_TRUE(!neither.OverridesStartup());
}

MT_TEST(parsed_alias_reports_invalid_input) {
    TemplateBuildSpec spec;
    spec.Alias("not valid!");

    const agentenv::tpl::TemplateBuildResult<Optional<SnapshotAlias> > parsed =
        spec.ParsedAlias();
    MT_EXPECT_TRUE(!parsed.ok());
    // A bad alias is the caller's fault, so it must not be a System error.
    MT_EXPECT_EQ(parsed.error().kind, TemplateBuildErrorKind::InvalidInput);
    MT_EXPECT_TRUE(parsed.error().ToString().find("invalid template build input") == 0);

    // No alias at all is a successful empty result, not an error.
    TemplateBuildSpec without;
    const agentenv::tpl::TemplateBuildResult<Optional<SnapshotAlias> > absent =
        without.ParsedAlias();
    MT_EXPECT_TRUE(absent.ok());
    MT_EXPECT_TRUE(!absent.value().has_value());
}

MT_TEST(steps_are_appended_in_order_with_their_payloads) {
    TemplateBuildSpec spec;
    spec.Run("echo hi")
        .Env("KEY", "value")
        .Workdir("/app")
        .User("nobody")
        .ExposedPort("8080")
        .Volume("/data")
        .Label("org.label", "v1");

    const std::vector<TemplateBuildStep>& steps = spec.Steps();
    MT_EXPECT_EQ(steps.size(), static_cast<std::size_t>(7));

    MT_EXPECT_EQ(steps[0].kind, TemplateBuildStepKind::Run);
    MT_EXPECT_EQ(steps[0].value, std::string("echo hi"));
    // Only Env and Label carry a key.
    MT_EXPECT_TRUE(steps[0].key.empty());

    MT_EXPECT_EQ(steps[1].kind, TemplateBuildStepKind::Env);
    MT_EXPECT_EQ(steps[1].key, std::string("KEY"));
    MT_EXPECT_EQ(steps[1].value, std::string("value"));

    MT_EXPECT_EQ(steps[2].kind, TemplateBuildStepKind::Workdir);
    MT_EXPECT_EQ(steps[2].value, std::string("/app"));
    MT_EXPECT_EQ(steps[3].kind, TemplateBuildStepKind::User);
    MT_EXPECT_EQ(steps[3].value, std::string("nobody"));
    MT_EXPECT_EQ(steps[4].kind, TemplateBuildStepKind::ExposedPort);
    MT_EXPECT_EQ(steps[4].value, std::string("8080"));
    MT_EXPECT_EQ(steps[5].kind, TemplateBuildStepKind::Volume);
    MT_EXPECT_EQ(steps[5].value, std::string("/data"));

    MT_EXPECT_EQ(steps[6].kind, TemplateBuildStepKind::Label);
    MT_EXPECT_EQ(steps[6].key, std::string("org.label"));
    MT_EXPECT_EQ(steps[6].value, std::string("v1"));

    // Nothing was stamped, so every step maps 1:1 to its position.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        MT_EXPECT_TRUE(!steps[i].source_step.has_value());
    }
}

MT_TEST(apt_builds_one_noninteractive_install_step) {
    TemplateBuildSpec spec;
    std::vector<std::string> packages;
    packages.push_back("curl");
    packages.push_back("git");
    spec.Apt(packages);

    MT_EXPECT_EQ(spec.Steps().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(spec.Steps()[0].kind, TemplateBuildStepKind::Run);
    MT_EXPECT_EQ(spec.Steps()[0].value,
                 std::string("DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install "
                             "-y --no-install-recommends curl git && rm -rf "
                             "/var/lib/apt/lists/*"));
}

MT_TEST(apt_drops_blank_packages_and_skips_empty_lists) {
    TemplateBuildSpec spec;
    std::vector<std::string> mixed;
    mixed.push_back("  ");
    mixed.push_back("curl");
    mixed.push_back("");
    mixed.push_back("\t\n");
    spec.Apt(mixed);

    MT_EXPECT_EQ(spec.Steps().size(), static_cast<std::size_t>(1));
    // Only the surviving package appears in the command.
    MT_EXPECT_TRUE(spec.Steps()[0].value.find("--no-install-recommends curl &&") !=
                   std::string::npos);

    // An all-blank list appends nothing rather than an install of nothing.
    TemplateBuildSpec blank;
    std::vector<std::string> all_blank;
    all_blank.push_back(" ");
    all_blank.push_back("");
    blank.Apt(all_blank);
    MT_EXPECT_EQ(blank.Steps().size(), static_cast<std::size_t>(0));

    TemplateBuildSpec none;
    none.Apt(std::vector<std::string>());
    MT_EXPECT_EQ(none.Steps().size(), static_cast<std::size_t>(0));
}

MT_TEST(stamp_source_steps_marks_only_the_expanded_tail) {
    // Models the e2b front-end expanding one request step into several
    // internal ENV steps: failed-step reporting must cite the request's
    // position, not the internal one.
    TemplateBuildSpec spec;
    spec.Run("echo first");
    const std::size_t before = spec.StepCount();

    spec.Env("A", "1").Env("B", "2").Env("C", "3");
    spec.StampSourceSteps(before, 2);

    const std::vector<TemplateBuildStep>& steps = spec.Steps();
    MT_EXPECT_EQ(steps.size(), static_cast<std::size_t>(4));
    // The earlier step is untouched.
    MT_EXPECT_TRUE(!steps[0].source_step.has_value());
    for (std::size_t i = before; i < steps.size(); ++i) {
        MT_EXPECT_TRUE(steps[i].source_step.has_value());
        MT_EXPECT_EQ(*steps[i].source_step, static_cast<std::size_t>(2));
    }
}

MT_TEST(stamping_from_the_end_changes_nothing) {
    TemplateBuildSpec spec;
    spec.Run("only");
    spec.StampSourceSteps(spec.StepCount(), 7);
    MT_EXPECT_TRUE(!spec.Steps()[0].source_step.has_value());
}

MT_TEST(rootfs_base_switches_between_ext4_and_overlaybd) {
    TemplateBuildSpec spec;
    spec.FromExistingRootfs("/tmp/rootfs.ext4");
    MT_EXPECT_TRUE(spec.RootfsBase()->is_ext4());

    // The last call wins: the two bases are mutually exclusive.
    spec.FromOverlaybdConfig("/tmp/image.json");
    MT_EXPECT_TRUE(spec.RootfsBase()->is_overlaybd());
    MT_EXPECT_EQ(spec.RootfsBase()->image_config_path, std::string("/tmp/image.json"));
    // `from_overlaybd_config` resolves with no extra configs.
    MT_EXPECT_TRUE(spec.RootfsBase()->image_configs.IsEmpty());
}

MT_TEST(resolved_overlaybd_image_carries_its_configs) {
    ImageConfigs configs;
    agentenv::core::JsonObject rootfs_config;
    rootfs_config["lowers"] = agentenv::core::Json(agentenv::core::JsonArray());
    // An unset drive id marks the rootfs entry.
    configs.Add(Optional<std::string>(), "/", agentenv::core::Json(rootfs_config));
    configs.Add(Optional<std::string>(std::string("vdc")), "/data",
                agentenv::core::Json(agentenv::core::JsonObject()));

    TemplateBuildSpec spec;
    spec.WithResolvedOverlaybdImage("/tmp/image.json", configs);

    MT_EXPECT_TRUE(spec.RootfsBase()->is_overlaybd());
    MT_EXPECT_EQ(spec.RootfsBase()->image_configs.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(!spec.RootfsBase()->image_configs.entries()[0].drive_id.has_value());
    MT_EXPECT_EQ(*spec.RootfsBase()->image_configs.entries()[1].drive_id, std::string("vdc"));
    MT_EXPECT_EQ(spec.RootfsBase()->image_configs.entries()[1].mount_path, std::string("/data"));
}

MT_TEST(spec_defaults_are_all_absent) {
    const TemplateBuildSpec spec;
    MT_EXPECT_TRUE(!spec.RootfsBase().has_value());
    MT_EXPECT_EQ(spec.StepCount(), static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(!spec.ResourcesRef().has_value());
    MT_EXPECT_TRUE(!spec.StartCmdRef().has_value());
    MT_EXPECT_TRUE(!spec.ReadyCmdRef().has_value());
    MT_EXPECT_TRUE(!spec.StartupShell().has_value());
    MT_EXPECT_TRUE(!spec.BaseContextRef().has_value());
    MT_EXPECT_TRUE(!spec.OverridesStartup());
}

MT_TEST(startup_shell_and_base_context_round_trip) {
    CommandContext context;
    context.workdir = "/app";
    context.env_vars["PATH"] = "/usr/bin";
    context.user = std::string("root");

    TemplateBuildSpec spec;
    spec.WithStartupShell("/bin/bash").WithBaseContext(context);

    MT_EXPECT_EQ(*spec.StartupShell(), std::string("/bin/bash"));
    MT_EXPECT_TRUE(spec.BaseContextRef().has_value());
    MT_EXPECT_EQ(spec.BaseContextRef()->workdir, std::string("/app"));
    MT_EXPECT_TRUE(*spec.BaseContextRef() == context);
}

int main() { return microtest::RunAll(); }
