// SPDX-License-Identifier: MIT
// Rust: src/template/builder.rs `mod tests` — the context-preparation and
// validation half (the execute/publish half needs a live build VM).
#include "agentenv/template/builder.h"

#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::tpl;  // NOLINT

namespace {

using agentenv::snapshot::CommittedSnapshot;
using agentenv::snapshot::SnapshotAlias;
using agentenv::snapshot::SnapshotRecord;
using agentenv::snapshot::StartupCommand;

/// Removes every workspace a test created, so a passing run leaves no
/// temporary directories behind.
class WorkspaceReaper {
 public:
    ~WorkspaceReaper() {
        for (std::size_t i = 0; i < paths_.size(); ++i) core::fs::RemoveDirAll(paths_[i]);
    }
    void Keep(const std::string& path) { paths_.push_back(path); }

 private:
    std::vector<std::string> paths_;
};

/// Rust test helper `SnapshotRecord::mock_ready(CommittedSnapshot::mock())`.
SnapshotRecord MockReady() {
    sandbox::SandboxResources resources;
    resources.cpu_count     = 2;
    resources.memory_mib    = 1024;
    resources.disk_size_mib = 4096;

    SnapshotRecord record = SnapshotRecord::TemplateWaiting(
        core::SnapshotId::Fresh(), core::Optional<SnapshotAlias>(), resources);

    CommittedSnapshot committed;
    committed.virtualization_mode = core::VirtualizationMode::Kvm;
    committed.context             = snapshot::CommandContext();
    record.committed              = core::Optional<CommittedSnapshot>(committed);
    return record;
}

/// A ready record whose committed half carries a startup command.
SnapshotRecord MockReadyWithStartup(const core::Optional<std::string>& shell) {
    SnapshotRecord record = MockReady();
    StartupCommand startup;
    startup.start_cmd = "echo base";
    startup.ready_cmd = "true";
    startup.shell     = shell;
    record.committed->startup = core::Optional<StartupCommand>(startup);
    return record;
}

/// An overlaybd base whose config file actually exists, since
/// `PrepareBaseRootfs` checks for it.
std::string WriteImageConfig(const std::string& dir) {
    MT_EXPECT_TRUE(core::fs::CreateDirAll(dir).ok());
    const std::string path = core::fs::Join(dir, "image.json");
    MT_EXPECT_TRUE(core::fs::Write(path, "{}").ok());
    return path;
}

}  // namespace

// ---- fresh context ---------------------------------------------------------

MT_TEST(builder_fresh_context_requires_a_rootfs_base) {
    // Rust `build_publish_requires_rootfs_base`.
    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(TemplateBuildSpec(), core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().kind == TemplateBuildErrorKind::InvalidInput);
    MT_EXPECT_TRUE(context.error().ToString().find("rootfs base is required") !=
                   std::string::npos);
}

MT_TEST(builder_fresh_context_rejects_an_ext4_base) {
    TemplateBuildSpec spec;
    spec.FromExistingRootfs("/tmp/does-not-matter.ext4");

    // Publishing is overlaybd-only; an ext4 base cannot be layered or shared.
    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().ToString().find("does not support ext4") != std::string::npos);
}

MT_TEST(builder_fresh_context_rejects_a_missing_image_config) {
    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig("/tmp/agentenv-absent/image.json");

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().kind == TemplateBuildErrorKind::InvalidInput);
    MT_EXPECT_TRUE(context.error().ToString().find("not found at") != std::string::npos);
}

MT_TEST(builder_fresh_context_populates_the_rootfs_base) {
    WorkspaceReaper reaper;
    const core::Expected<std::string, std::string> root =
        core::fs::CreateTempDir("agentenv-builder-");
    MT_EXPECT_TRUE(root.ok());
    reaper.Keep(root.value());
    const std::string config = WriteImageConfig(root.value());

    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig(config);
    spec.Run("make");

    const core::SnapshotId id = core::SnapshotId::Fresh();
    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, id);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(context.value().build_snapshot_id == id);
    MT_EXPECT_TRUE(context.value().base.kind == TemplateBuildBase::Kind::Rootfs);
    MT_EXPECT_EQ(context.value().base.launch_rootfs_path, config);
    MT_EXPECT_EQ(context.value().steps.size(), static_cast<std::size_t>(1));
    // The workspace is a real directory the build can write into.
    MT_EXPECT_TRUE(core::fs::Exists(context.value().workspace));
}

MT_TEST(builder_fresh_context_attaches_a_ublk_config_to_the_base) {
    WorkspaceReaper reaper;
    const core::Expected<std::string, std::string> root =
        core::fs::CreateTempDir("agentenv-builder-");
    MT_EXPECT_TRUE(root.ok());
    reaper.Keep(root.value());
    const std::string config = WriteImageConfig(root.value());

    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig(config);

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(context.value().base.ublk_config.has_value());
    MT_EXPECT_EQ(context.value().base.ublk_config->overlaybd.image_config_path, config);
    MT_EXPECT_TRUE(context.value().base.ublk_config->backend ==
                   sandbox::ublk::UblkBackendKind::Overlaybd);
}

MT_TEST(builder_fresh_context_falls_back_to_default_resources) {
    WorkspaceReaper reaper;
    const core::Expected<std::string, std::string> root =
        core::fs::CreateTempDir("agentenv-builder-");
    MT_EXPECT_TRUE(root.ok());
    reaper.Keep(root.value());

    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig(WriteImageConfig(root.value()));

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    // The machine defaults, with the disk left unsized: its size is only known
    // after the build measures the produced rootfs.
    MT_EXPECT_TRUE(context.value().resources.cpu_count > 0);
    MT_EXPECT_TRUE(context.value().resources.memory_mib > 0);
    MT_EXPECT_EQ(context.value().resources.disk_size_mib, static_cast<uint32_t>(0));
}

MT_TEST(builder_fresh_context_carries_the_cpu_config) {
    WorkspaceReaper reaper;
    const core::Expected<std::string, std::string> root =
        core::fs::CreateTempDir("agentenv-builder-");
    MT_EXPECT_TRUE(root.ok());
    reaper.Keep(root.value());

    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig(WriteImageConfig(root.value()));

    // The cluster CPU intersection keeps the built template runnable on every
    // node, not only the one that built it.
    const TemplateBuilder builder =
        TemplateBuilder::WithCpuConfig(core::Optional<std::string>(std::string("{\"cpu\":1}")));
    const TemplateBuildResult<TemplateBuildContext> context =
        builder.PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(context.value().cpu_config_json.has_value());
    MT_EXPECT_EQ(*context.value().cpu_config_json, std::string("{\"cpu\":1}"));

    // A builder without one leaves it unset rather than inventing a value.
    const TemplateBuildResult<TemplateBuildContext> plain =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(plain.ok());
    reaper.Keep(plain.value().workspace);
    MT_EXPECT_TRUE(!plain.value().cpu_config_json.has_value());
}

MT_TEST(builder_fresh_context_rejects_an_invalid_alias) {
    WorkspaceReaper reaper;
    const core::Expected<std::string, std::string> root =
        core::fs::CreateTempDir("agentenv-builder-");
    MT_EXPECT_TRUE(root.ok());
    reaper.Keep(root.value());

    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig(WriteImageConfig(root.value()));
    spec.Alias("bad/alias");

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareFreshContext(spec, core::SnapshotId::Fresh());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().kind == TemplateBuildErrorKind::InvalidInput);
}

// ---- snapshot-based context ------------------------------------------------

MT_TEST(builder_snapshot_context_rejects_reusing_the_base_id) {
    // Rust `snapshot_base_context_rejects_reusing_base_snapshot_id`. Building
    // onto itself would publish over the record the build resumes from.
    const SnapshotRecord base = MockReady();
    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(), base.id, base);
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().ToString().find("must differ from the base snapshot id") !=
                   std::string::npos);
}

MT_TEST(builder_snapshot_context_rejects_overriding_the_rootfs_base) {
    // Rust `build_snapshot_base_rejects_overriding_rootfs_base`. The base *is*
    // the rootfs, so a spec-supplied one would be silently ignored.
    TemplateBuildSpec spec;
    spec.FromOverlaybdConfig("/tmp/whatever.json");

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(spec, core::SnapshotId::Fresh(), MockReady());
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().ToString().find("must not override the rootfs base") !=
                   std::string::npos);
}

MT_TEST(builder_snapshot_context_rejects_a_resource_change) {
    // Rust `build_snapshot_base_rejects_resource_change`. The CPU/memory shape
    // is baked into the capture the build resumes.
    const SnapshotRecord base = MockReady();
    TemplateBuildSpec spec;
    spec.Resources(base.resources.cpu_count + 1, base.resources.memory_mib);

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(spec, core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().ToString().find("cannot change CPU or memory") !=
                   std::string::npos);
}

MT_TEST(builder_snapshot_context_accepts_matching_resources) {
    WorkspaceReaper reaper;
    const SnapshotRecord base = MockReady();
    TemplateBuildSpec spec;
    // Restating the same shape is not a change, so it is allowed.
    spec.Resources(base.resources.cpu_count, base.resources.memory_mib);

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(spec, core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);
    // The base's resources win, including the disk size the capture recorded.
    MT_EXPECT_EQ(context.value().resources.disk_size_mib, base.resources.disk_size_mib);
}

MT_TEST(builder_snapshot_context_rejects_another_virtualization_mode) {
    // Rust `snapshot_base_context_rejects_other_virtualization_mode`. A
    // capture is only resumable under the ABI it was taken with.
    SnapshotRecord base = MockReady();
    base.committed->virtualization_mode = core::VirtualizationMode::Pvm;

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(),
                                                     core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().kind == TemplateBuildErrorKind::InvalidInput);
    MT_EXPECT_TRUE(context.error().ToString().find("virtualization mode") != std::string::npos);
    // The message names both modes so the operator can tell which node to use.
    MT_EXPECT_TRUE(context.error().ToString().find("this node runs in mode") !=
                   std::string::npos);
}

MT_TEST(builder_snapshot_context_requires_committed_artifacts) {
    sandbox::SandboxResources resources;
    // A waiting record has nothing to resume from.
    const SnapshotRecord waiting = SnapshotRecord::TemplateWaiting(
        core::SnapshotId::Fresh(), core::Optional<SnapshotAlias>(), resources);

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(),
                                                     core::SnapshotId::Fresh(), waiting);
    MT_EXPECT_TRUE(!context.ok());
    MT_EXPECT_TRUE(context.error().kind == TemplateBuildErrorKind::InvalidInput);
}

MT_TEST(builder_snapshot_context_inherits_base_context_but_not_alias) {
    // Rust `snapshot_base_context_inherits_base_context_but_not_alias`.
    WorkspaceReaper reaper;
    SnapshotRecord base = MockReady();
    base.committed->context.WithEnvVar("FROM_BASE", "1");
    base.committed->context.WithWorkdir("/base");

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(),
                                                     core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    // The environment and workdir carry over...
    MT_EXPECT_EQ(context.value().initial_context.env_vars.find("FROM_BASE")->second,
                 std::string("1"));
    MT_EXPECT_EQ(context.value().initial_context.workdir, std::string("/base"));
    // ...but the alias does not: it names a specific template, and inheriting
    // it would let the derived build claim the base's name.
    MT_EXPECT_TRUE(!context.value().alias.has_value());
}

MT_TEST(builder_snapshot_context_keeps_the_base_as_the_build_base) {
    WorkspaceReaper reaper;
    const SnapshotRecord base = MockReady();
    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(),
                                                     core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(context.value().base.kind == TemplateBuildBase::Kind::Snapshot);
    MT_EXPECT_TRUE(context.value().base.base_snapshot.id == base.id);
    // The recorded mode is the capture's own, which was just proven to match
    // the node's.
    MT_EXPECT_TRUE(context.value().virtualization_mode ==
                   base.committed->virtualization_mode);
}

MT_TEST(builder_snapshot_context_inherits_base_startup_when_not_overridden) {
    // Rust `snapshot_base_context_inherits_base_startup_when_not_overridden`.
    WorkspaceReaper reaper;
    const SnapshotRecord base =
        MockReadyWithStartup(core::Optional<std::string>(std::string("/bin/sh")));

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(TemplateBuildSpec(),
                                                     core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(!context.value().override_startup);
    MT_EXPECT_TRUE(context.value().startup.has_value());
    MT_EXPECT_EQ(context.value().startup->start_cmd, std::string("echo base"));
    MT_EXPECT_EQ(context.value().startup->ready_cmd, std::string("true"));
}

MT_TEST(builder_snapshot_context_overrides_base_startup_when_set) {
    // Rust `snapshot_base_context_overrides_base_startup_when_command_is_set`.
    WorkspaceReaper reaper;
    const SnapshotRecord base =
        MockReadyWithStartup(core::Optional<std::string>(std::string("/bin/sh")));
    TemplateBuildSpec spec;
    spec.StartCmd("echo derived");

    const TemplateBuildResult<TemplateBuildContext> context =
        TemplateBuilder().PrepareSnapshotBaseContext(spec, core::SnapshotId::Fresh(), base);
    MT_EXPECT_TRUE(context.ok());
    reaper.Keep(context.value().workspace);

    MT_EXPECT_TRUE(context.value().override_startup);
    MT_EXPECT_EQ(context.value().startup->start_cmd, std::string("echo derived"));
    // The un-overridden half is blank, not inherited: an override replaces the
    // whole startup command rather than merging field-by-field.
    MT_EXPECT_EQ(context.value().startup->ready_cmd, std::string(""));
}

MT_TEST(builder_startup_override_inherits_the_shell_unless_set) {
    // Rust `startup_command_overrides_inherit_shell_unless_explicitly_set`,
    // as the same matrix: base shell x explicit shell x which half overrides.
    WorkspaceReaper reaper;

    for (int base_shell_case = 0; base_shell_case < 3; ++base_shell_case) {
        for (int explicit_shell_case = 0; explicit_shell_case < 2; ++explicit_shell_case) {
            for (int start_override = 0; start_override < 2; ++start_override) {
                // 0: no startup at all, 1: startup with no shell, 2: shell set.
                SnapshotRecord base = MockReady();
                if (base_shell_case == 1) {
                    base = MockReadyWithStartup(core::Optional<std::string>());
                } else if (base_shell_case == 2) {
                    base = MockReadyWithStartup(
                        core::Optional<std::string>(std::string("/bin/sh")));
                }

                TemplateBuildSpec spec;
                if (start_override != 0) {
                    spec.StartCmd("echo derived");
                } else {
                    spec.ReadyCmd("test -f /ready");
                }
                if (explicit_shell_case != 0) spec.WithStartupShell("/bin/ash");

                const TemplateBuildResult<TemplateBuildContext> context =
                    TemplateBuilder().PrepareSnapshotBaseContext(
                        spec, core::SnapshotId::Fresh(), base);
                MT_EXPECT_TRUE(context.ok());
                reaper.Keep(context.value().workspace);
                MT_EXPECT_TRUE(context.value().startup.has_value());

                // An explicit shell wins; otherwise the base's shell is
                // inherited, because the base image may only be usable under
                // it. Only when neither exists is the shell left unset.
                const core::Optional<std::string>& shell = context.value().startup->shell;
                if (explicit_shell_case != 0) {
                    MT_EXPECT_TRUE(shell.has_value());
                    MT_EXPECT_EQ(*shell, std::string("/bin/ash"));
                } else if (base_shell_case == 2) {
                    MT_EXPECT_TRUE(shell.has_value());
                    MT_EXPECT_EQ(*shell, std::string("/bin/sh"));
                } else {
                    MT_EXPECT_TRUE(!shell.has_value());
                }

                MT_EXPECT_EQ(context.value().startup->start_cmd,
                             start_override != 0 ? std::string("echo derived") : std::string(""));
                MT_EXPECT_EQ(context.value().startup->ready_cmd,
                             start_override != 0 ? std::string("")
                                                 : std::string("test -f /ready"));
            }
        }
    }
}

// ---- helpers ---------------------------------------------------------------

MT_TEST(builder_startup_from_spec_is_absent_without_an_override) {
    // A spec that overrides nothing must not install an empty startup command
    // over the base image's.
    MT_EXPECT_TRUE(!TemplateBuilder::StartupFromSpec(TemplateBuildSpec()).has_value());

    TemplateBuildSpec only_shell;
    only_shell.WithStartupShell("/bin/ash");
    // A shell alone is not an override: there is no command to run under it.
    MT_EXPECT_TRUE(!TemplateBuilder::StartupFromSpec(only_shell).has_value());

    TemplateBuildSpec with_start;
    with_start.StartCmd("echo hi");
    MT_EXPECT_TRUE(TemplateBuilder::StartupFromSpec(with_start).has_value());
}

MT_TEST(builder_default_resources_leave_the_disk_unsized) {
    const sandbox::SandboxResources resources = TemplateBuilder::DefaultSandboxResources();
    MT_EXPECT_TRUE(resources.cpu_count > 0);
    MT_EXPECT_TRUE(resources.memory_mib > 0);
    MT_EXPECT_EQ(resources.disk_size_mib, static_cast<uint32_t>(0));
}

int main() { return microtest::RunAll(); }
