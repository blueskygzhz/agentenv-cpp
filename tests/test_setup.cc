// SPDX-License-Identifier: MIT
// Rust: src/setup/{mod,kvm,ublk,network_capacity}.rs test modules, plus
// coverage for the process helper they all share.
#include <sstream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/process.h"
#include "agentenv/setup.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
using agentenv::core::VirtualizationMode;
namespace fs = agentenv::core::fs;
namespace process = agentenv::core::process;
namespace netcap = agentenv::setup::network_capacity;
namespace setup = agentenv::setup;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-setup-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }

    /// Writes a fake sysctl under this root, creating parents.
    void WriteSysctl(const std::string& relative, const std::string& value) const {
        const std::string full = fs::Join(path, relative);
        const Optional<std::string> parent = fs::Parent(full);
        MT_EXPECT_TRUE(parent.has_value());
        MT_EXPECT_TRUE(fs::CreateDirAll(*parent).ok());
        MT_EXPECT_TRUE(fs::Write(full, value).ok());
    }

    /// Populates every threshold at or above its recommendation.
    void WriteAllSysctlsAtRecommended() const {
        const std::vector<netcap::CapacityThreshold>& thresholds = netcap::Thresholds();
        for (std::size_t i = 0; i < thresholds.size(); ++i) {
            std::ostringstream value;
            value << thresholds[i].recommended << "\n";
            WriteSysctl(thresholds[i].relative_path, value.str());
        }
    }
};

/// Finds a threshold by sysctl name.
const netcap::CapacityThreshold& ThresholdNamed(const std::string& name) {
    const std::vector<netcap::CapacityThreshold>& thresholds = netcap::Thresholds();
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        if (thresholds[i].name == name) return thresholds[i];
    }
    MT_EXPECT_TRUE(false);  // unreachable: the caller named a real threshold
    return thresholds[0];
}

bool AnyContains(const std::vector<std::string>& lines, const std::string& needle) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// core::process
// ---------------------------------------------------------------------------

MT_TEST(process_run_captures_stdout_and_status) {
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf hello");

    const agentenv::core::Expected<process::Output, std::string> output = process::Run(argv);
    MT_EXPECT_TRUE(output.ok());
    MT_EXPECT_TRUE(output.value().success());
    MT_EXPECT_EQ(output.value().stdout_text, std::string("hello"));
    MT_EXPECT_EQ(output.value().stderr_text, std::string(""));
}

MT_TEST(process_run_captures_stderr_separately) {
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("printf out; printf err >&2; exit 3");

    const agentenv::core::Expected<process::Output, std::string> output = process::Run(argv);
    MT_EXPECT_TRUE(output.ok());
    // A non-zero exit is reported through the status, not as a failure.
    MT_EXPECT_TRUE(!output.value().success());
    MT_EXPECT_EQ(output.value().code, 3);
    MT_EXPECT_EQ(output.value().stdout_text, std::string("out"));
    MT_EXPECT_EQ(output.value().stderr_text, std::string("err"));
    MT_EXPECT_EQ(output.value().StatusString(), std::string("exit status: 3"));
}

MT_TEST(process_run_handles_output_larger_than_a_pipe_buffer) {
    // Reading the two pipes sequentially would deadlock here: the child fills
    // one while we wait on the other. This is the regression guard for that.
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back("i=0; while [ $i -lt 400 ]; do "
                   "printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; "
                   "printf 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' >&2; "
                   "i=$((i+1)); done");

    const agentenv::core::Expected<process::Output, std::string> output = process::Run(argv);
    MT_EXPECT_TRUE(output.ok());
    MT_EXPECT_EQ(output.value().stdout_text.size(), static_cast<std::size_t>(16000));
    MT_EXPECT_EQ(output.value().stderr_text.size(), static_cast<std::size_t>(16000));
}

MT_TEST(process_run_reports_a_missing_program_as_exit_127) {
    std::vector<std::string> argv;
    argv.push_back("aenv-definitely-not-a-real-binary");

    const agentenv::core::Expected<process::Output, std::string> output = process::Run(argv);
    // The fork succeeded, so this is a status, not an error — same as a shell.
    MT_EXPECT_TRUE(output.ok());
    MT_EXPECT_EQ(output.value().code, 127);

    MT_EXPECT_TRUE(!process::Run(std::vector<std::string>()).ok());
}

MT_TEST(process_argv_is_not_shell_interpreted) {
    // argv goes straight to execvp, so metacharacters are literal data. If
    // this ever regressed into a shell, a filename could inject a command.
    std::vector<std::string> argv;
    argv.push_back("printf");
    argv.push_back("%s");
    argv.push_back("a; rm -rf /tmp/nope | b $(whoami)");

    const agentenv::core::Expected<process::Output, std::string> output = process::Run(argv);
    MT_EXPECT_TRUE(output.ok());
    MT_EXPECT_EQ(output.value().stdout_text,
                 std::string("a; rm -rf /tmp/nope | b $(whoami)"));
}

MT_TEST(process_which_finds_and_rejects) {
    // `sh` must exist on any host that can run these tests.
    const Optional<std::string> sh = process::Which("sh");
    MT_EXPECT_TRUE(sh.has_value());
    MT_EXPECT_TRUE(fs::IsFile(*sh));
    MT_EXPECT_TRUE(process::Exists("sh"));

    MT_EXPECT_TRUE(!process::Which("aenv-definitely-not-a-real-binary").has_value());
    MT_EXPECT_TRUE(!process::Which("").has_value());
    // A directory carries the execute bit but is not a program.
    MT_EXPECT_TRUE(!process::Which("/tmp").has_value());
}

MT_TEST(process_trimmed_stdout_strips_surrounding_whitespace) {
    process::Output output;
    output.stdout_text = "  6.1.0-generic \n";
    MT_EXPECT_EQ(process::TrimmedStdout(output), std::string("6.1.0-generic"));

    output.stdout_text = " \n\t ";
    MT_EXPECT_EQ(process::TrimmedStdout(output), std::string(""));
}

// ---------------------------------------------------------------------------
// setup/kvm
// ---------------------------------------------------------------------------

MT_TEST(kvm_mode_requires_pvm_module_to_be_absent) {
    // Rust: `kvm_mode_requires_pvm_module_to_be_absent`.
    MT_EXPECT_TRUE(setup::kvm::ValidateMode(VirtualizationMode::Kvm, "x86_64", false).ok());

    const agentenv::core::Expected<Unit, std::string> conflict =
        setup::kvm::ValidateMode(VirtualizationMode::Kvm, "x86_64", true);
    MT_EXPECT_TRUE(!conflict.ok());
    MT_EXPECT_TRUE(conflict.error().find("kvm_pvm module is loaded") != std::string::npos);
}

MT_TEST(pvm_mode_requires_x86_64_and_pvm_module) {
    // Rust: `pvm_mode_requires_x86_64_and_pvm_module`.
    MT_EXPECT_TRUE(setup::kvm::ValidateMode(VirtualizationMode::Pvm, "x86_64", true).ok());

    const agentenv::core::Expected<Unit, std::string> wrong_arch =
        setup::kvm::ValidateMode(VirtualizationMode::Pvm, "aarch64", true);
    MT_EXPECT_TRUE(!wrong_arch.ok());
    MT_EXPECT_TRUE(wrong_arch.error().find("only supported on x86_64") != std::string::npos);

    const agentenv::core::Expected<Unit, std::string> no_module =
        setup::kvm::ValidateMode(VirtualizationMode::Pvm, "x86_64", false);
    MT_EXPECT_TRUE(!no_module.ok());
    MT_EXPECT_TRUE(no_module.error().find("requires the kvm_pvm host module") !=
                   std::string::npos);
}

MT_TEST(kvm_mode_on_non_x86_is_allowed) {
    // Only PVM is x86-only; KVM works on aarch64 hosts.
    MT_EXPECT_TRUE(setup::kvm::ValidateMode(VirtualizationMode::Kvm, "aarch64", false).ok());
}

MT_TEST(host_arch_is_one_of_the_supported_names) {
    const std::string arch = setup::kvm::HostArch();
    MT_EXPECT_TRUE(arch == "x86_64" || arch == "aarch64" || arch == "unknown");
}

// ---------------------------------------------------------------------------
// setup/ublk
// ---------------------------------------------------------------------------

MT_TEST(udev_rule_content_covers_all_three_device_classes) {
    const std::string rule = setup::ublk::UdevRuleContent("aenv");

    MT_EXPECT_TRUE(rule.find("# Managed by agentenv server setup") == 0);
    // The control device plus both block/char device families must be granted.
    MT_EXPECT_TRUE(rule.find("KERNEL==\"ublk-control\", MODE=\"0660\", GROUP=\"aenv\"") !=
                   std::string::npos);
    MT_EXPECT_TRUE(rule.find("KERNEL==\"ublkc*\", MODE=\"0660\", GROUP=\"aenv\"") !=
                   std::string::npos);
    MT_EXPECT_TRUE(rule.find("KERNEL==\"ublkb*\", MODE=\"0660\", GROUP=\"aenv\"") !=
                   std::string::npos);
    // 0660, never 0666: access is granted to a group, not to the world.
    MT_EXPECT_TRUE(rule.find("0666") == std::string::npos);
    MT_EXPECT_EQ(rule[rule.size() - 1], '\n');
}

MT_TEST(write_udev_rule_installs_into_an_existing_directory) {
    TempRoot root;
    const std::string rules_dir = root.Join("rules.d");
    MT_EXPECT_TRUE(fs::CreateDirAll(rules_dir).ok());

    MT_EXPECT_TRUE(setup::ublk::WriteUdevRule(rules_dir, "aenv").ok());

    const std::string installed = fs::Join(rules_dir, "99-agentenv-ublk.rules");
    MT_EXPECT_TRUE(fs::Exists(installed));
    MT_EXPECT_EQ(fs::ReadToString(installed).value(), setup::ublk::UdevRuleContent("aenv"));
}

MT_TEST(write_udev_rule_skips_a_host_without_udev) {
    TempRoot root;
    const std::string absent = root.Join("no-such-rules.d");

    // Rust warns and returns Ok: a host without udev is not a setup failure.
    MT_EXPECT_TRUE(setup::ublk::WriteUdevRule(absent, "aenv").ok());
    MT_EXPECT_TRUE(!fs::Exists(absent));
}

// ---------------------------------------------------------------------------
// setup/mod — runtime account validation
// ---------------------------------------------------------------------------

MT_TEST(runtime_account_names_match_installer_validation) {
    // Rust: `runtime_account_names_match_installer_validation`.
    const char* const valid[] = {"aenv", "_aenv", "aenv-1", "aenv_service"};
    for (std::size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        MT_EXPECT_TRUE(setup::IsValidRuntimeAccountName(valid[i]));
    }

    const char* const invalid[] = {"", "Aenv", "a.env", "-aenv", "aenv service"};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        MT_EXPECT_TRUE(!setup::IsValidRuntimeAccountName(invalid[i]));
    }
}

MT_TEST(runtime_account_names_reject_injection_shaped_input) {
    // These names get interpolated into udev rules and usermod arguments, so
    // the validation is a security boundary, not just tidiness.
    const char* const rejected[] = {"aenv;reboot", "aenv$(id)", "aenv`id`", "aenv\nroot",
                                    "aenv\"x", "aenv/../root", "1aenv", "aenv*"};
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!setup::IsValidRuntimeAccountName(rejected[i]));
    }
}

MT_TEST(validate_runtime_account_rejects_invalid_names_before_lookup) {
    const agentenv::core::Expected<setup::RuntimeAccount, std::string> invalid =
        setup::ValidateRuntimeAccount("Bad Name", "aenv");
    MT_EXPECT_TRUE(!invalid.ok());
    MT_EXPECT_TRUE(invalid.error().find("must match [a-z_][a-z0-9_-]*") != std::string::npos);
}

MT_TEST(validate_runtime_account_rejects_a_missing_account) {
    const agentenv::core::Expected<setup::RuntimeAccount, std::string> missing =
        setup::ValidateRuntimeAccount("aenv-no-such-user-here", "aenv-no-such-group-here");
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().find("does not exist") != std::string::npos);
}

MT_TEST(validate_runtime_account_rejects_root) {
    // root exists on every host, so this exercises the non-root requirement.
    const agentenv::core::Expected<setup::RuntimeAccount, std::string> as_root =
        setup::ValidateRuntimeAccount("root", "root");
    MT_EXPECT_TRUE(!as_root.ok());
    MT_EXPECT_TRUE(as_root.error().find("must be non-root") != std::string::npos);
}

MT_TEST(host_provisioning_requires_root) {
    if (fs::EffectiveUidIsRoot()) {
        // Running as root: the guard passes and validation is what rejects.
        const agentenv::core::Expected<setup::RuntimeAccount, std::string> checked =
            setup::CheckHostProvisioningPreconditions("aenv-no-such-user-here", "aenv");
        MT_EXPECT_TRUE(!checked.ok());
        MT_EXPECT_TRUE(checked.error().find("requires root") == std::string::npos);
        return;
    }
    const agentenv::core::Expected<setup::RuntimeAccount, std::string> checked =
        setup::CheckHostProvisioningPreconditions("aenv", "aenv");
    MT_EXPECT_TRUE(!checked.ok());
    MT_EXPECT_TRUE(checked.error().find("requires root; rerun with sudo") != std::string::npos);
}

// ---------------------------------------------------------------------------
// setup/network_capacity
// ---------------------------------------------------------------------------

MT_TEST(thresholds_cover_the_documented_kernel_parameters) {
    const std::vector<netcap::CapacityThreshold>& thresholds = netcap::Thresholds();
    MT_EXPECT_EQ(thresholds.size(), static_cast<std::size_t>(6));

    // Order matters: it is the order written into 99-aenv.conf.
    MT_EXPECT_EQ(thresholds[0].name, std::string("net.ipv4.neigh.default.gc_thresh3"));
    MT_EXPECT_EQ(thresholds[0].recommended, static_cast<uint64_t>(16384));
    MT_EXPECT_EQ(thresholds[5].name, std::string("fs.inotify.max_user_instances"));
    MT_EXPECT_EQ(thresholds[5].recommended, static_cast<uint64_t>(8192));

    // Only nf_conntrack_max tolerates an unreadable sysctl, because the module
    // may not be loaded until conntrack is first used.
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        const bool optional = thresholds[i].optional_read_failure_reason.has_value();
        MT_EXPECT_EQ(optional, thresholds[i].name == "net.netfilter.nf_conntrack_max");
        // Paths must stay relative so they can be rerooted.
        MT_EXPECT_TRUE(thresholds[i].relative_path[0] != '/');
    }
}

MT_TEST(inspect_passes_when_every_threshold_is_met) {
    TempRoot root;
    root.WriteAllSysctlsAtRecommended();

    const netcap::CapacityReport report = netcap::Inspect(root.path, false);
    MT_EXPECT_TRUE(report.passed());
    MT_EXPECT_EQ(report.warnings.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.remediation_commands.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(report.adjusted.size(), static_cast<std::size_t>(0));
}

MT_TEST(inspect_warns_and_suggests_a_command_for_a_low_value) {
    TempRoot root;
    root.WriteAllSysctlsAtRecommended();
    // Drop one below its recommendation.
    root.WriteSysctl(ThresholdNamed("kernel.pid_max").relative_path, "4096\n");

    const netcap::CapacityReport report = netcap::Inspect(root.path, false);
    MT_EXPECT_TRUE(!report.passed());
    MT_EXPECT_EQ(report.warnings.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(AnyContains(report.warnings, "kernel.pid_max: current=4096"));
    MT_EXPECT_TRUE(AnyContains(report.warnings, "recommended=4194304"));
    // The reason is included so the warning explains itself.
    MT_EXPECT_TRUE(AnyContains(report.warnings, "process ID exhaustion"));
    MT_EXPECT_TRUE(AnyContains(report.remediation_commands,
                               "sysctl -w kernel.pid_max=4194304"));
    // Read-only inspection must not change anything.
    MT_EXPECT_EQ(report.adjusted.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(netcap::ReadSysctl(root.Join(
                     ThresholdNamed("kernel.pid_max").relative_path)).value(),
                 static_cast<uint64_t>(4096));
}

MT_TEST(inspect_raises_a_low_value_when_adjusting) {
    TempRoot root;
    root.WriteAllSysctlsAtRecommended();
    const netcap::CapacityThreshold& threshold = ThresholdNamed("fs.inotify.max_user_instances");
    root.WriteSysctl(threshold.relative_path, "128\n");

    const netcap::CapacityReport report = netcap::Inspect(root.path, true);
    MT_EXPECT_TRUE(report.passed());  // the adjustment succeeded
    MT_EXPECT_EQ(report.adjusted.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(AnyContains(report.adjusted, "fs.inotify.max_user_instances: 128 -> 8192"));

    // The value on "disk" actually changed.
    MT_EXPECT_EQ(netcap::ReadSysctl(root.Join(threshold.relative_path)).value(),
                 threshold.recommended);
}

MT_TEST(inspect_skips_an_optional_unreadable_threshold) {
    TempRoot root;
    root.WriteAllSysctlsAtRecommended();
    // nf_conntrack_max absent: expected when the module is not loaded.
    MT_EXPECT_TRUE(
        fs::RemoveFile(root.Join(ThresholdNamed("net.netfilter.nf_conntrack_max")
                                     .relative_path))
            .ok());

    const netcap::CapacityReport report = netcap::Inspect(root.path, false);
    MT_EXPECT_TRUE(report.passed());
    MT_EXPECT_TRUE(!AnyContains(report.warnings, "nf_conntrack"));
}

MT_TEST(inspect_warns_about_a_required_unreadable_threshold) {
    TempRoot root;
    root.WriteAllSysctlsAtRecommended();
    // pid_max is not optional, so its absence must be reported.
    MT_EXPECT_TRUE(
        fs::RemoveFile(root.Join(ThresholdNamed("kernel.pid_max").relative_path)).ok());

    const netcap::CapacityReport report = netcap::Inspect(root.path, false);
    MT_EXPECT_TRUE(!report.passed());
    MT_EXPECT_TRUE(AnyContains(report.warnings, "kernel.pid_max: unable to read"));
    // An unreadable parameter has no current value, so no `sysctl -w` is
    // suggested for it.
    MT_EXPECT_TRUE(!AnyContains(report.remediation_commands, "kernel.pid_max"));
}

MT_TEST(read_sysctl_rejects_non_numeric_content) {
    TempRoot root;
    root.WriteSysctl("bad", "not-a-number\n");
    MT_EXPECT_TRUE(!netcap::ReadSysctl(root.Join("bad")).ok());

    root.WriteSysctl("empty", "\n");
    MT_EXPECT_TRUE(!netcap::ReadSysctl(root.Join("empty")).ok());

    // Overflow must fail rather than wrap into a small, acceptable-looking
    // value.
    root.WriteSysctl("huge", "99999999999999999999999999\n");
    MT_EXPECT_TRUE(!netcap::ReadSysctl(root.Join("huge")).ok());

    root.WriteSysctl("good", "  16384 \n");
    MT_EXPECT_EQ(netcap::ReadSysctl(root.Join("good")).value(),
                 static_cast<uint64_t>(16384));
}

MT_TEST(persistent_config_enables_forwarding_and_every_threshold) {
    const std::string content = netcap::PersistentConfigContent();

    MT_EXPECT_TRUE(content.find("# Managed by AENV host setup") == 0);
    MT_EXPECT_TRUE(content.find("net.ipv4.ip_forward = 1") != std::string::npos);

    const std::vector<netcap::CapacityThreshold>& thresholds = netcap::Thresholds();
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        std::ostringstream line;
        line << thresholds[i].name << " = " << thresholds[i].recommended;
        MT_EXPECT_TRUE(content.find(line.str()) != std::string::npos);
    }
}

MT_TEST(install_persistent_config_creates_the_sysctl_drop_in) {
    TempRoot root;
    MT_EXPECT_TRUE(netcap::InstallPersistentConfig(root.path).ok());

    const std::string installed = root.Join(netcap::kSysctlConfRelativePath);
    MT_EXPECT_TRUE(fs::Exists(installed));
    MT_EXPECT_EQ(fs::ReadToString(installed).value(), netcap::PersistentConfigContent());
}

MT_TEST(env_truthy_accepts_only_the_documented_spellings) {
    const char* const name = "AENV_TEST_TRUTHY_PROBE";

    const char* const truthy[] = {"1", "true", "TRUE", "yes", "YES", "on", "ON"};
    for (std::size_t i = 0; i < sizeof(truthy) / sizeof(truthy[0]); ++i) {
        MT_EXPECT_EQ(::setenv(name, truthy[i], 1), 0);
        MT_EXPECT_TRUE(netcap::EnvTruthy(name));
    }

    // Notably "True" and "Yes" are *not* accepted upstream.
    const char* const falsy[] = {"0", "false", "no", "off", "", "True", "Yes", "2"};
    for (std::size_t i = 0; i < sizeof(falsy) / sizeof(falsy[0]); ++i) {
        MT_EXPECT_EQ(::setenv(name, falsy[i], 1), 0);
        MT_EXPECT_TRUE(!netcap::EnvTruthy(name));
    }

    MT_EXPECT_EQ(::unsetenv(name), 0);
    MT_EXPECT_TRUE(!netcap::EnvTruthy(name));
}

MT_TEST(force_env_overrides_the_container_skip) {
    // Inside a container the sysctls belong to the host, so tuning is skipped
    // unless the operator explicitly forces it.
    MT_EXPECT_EQ(::setenv(netcap::kForceSysctlEnv, "1", 1), 0);
    MT_EXPECT_TRUE(!netcap::ShouldSkipSysctlTuning());

    MT_EXPECT_EQ(::unsetenv(netcap::kForceSysctlEnv), 0);
    // Without the override the decision follows container detection.
    MT_EXPECT_EQ(netcap::ShouldSkipSysctlTuning(), netcap::RunningInContainer());
}

// ---------------------------------------------------------------------------
// setup/mod — environment checks
// ---------------------------------------------------------------------------

MT_TEST(check_environment_reports_every_step) {
    // The checks touch the real host, so the outcomes are not asserted — only
    // that all three steps are reported, each with a detail when it fails.
    const std::vector<setup::EnvironmentCheck> checks =
        setup::CheckEnvironment(VirtualizationMode::Kvm);
    MT_EXPECT_EQ(checks.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(checks[0].step, std::string("kvm"));
    MT_EXPECT_EQ(checks[1].step, std::string("ublk"));
    MT_EXPECT_EQ(checks[2].step, std::string("ip_forward"));

    for (std::size_t i = 0; i < checks.size(); ++i) {
        // A failing step must explain itself; a passing one needs no detail.
        if (!checks[i].ok) MT_EXPECT_TRUE(!checks[i].detail.empty());
    }
}

MT_TEST(raise_file_descriptor_limit_never_exceeds_the_hard_limit) {
    const agentenv::core::Expected<uint64_t, std::string> limit =
        setup::RaiseFileDescriptorLimit();
    MT_EXPECT_TRUE(limit.ok());

    struct rlimit current;
    MT_EXPECT_EQ(::getrlimit(RLIMIT_NOFILE, &current), 0);
    MT_EXPECT_TRUE(limit.value() <= static_cast<uint64_t>(current.rlim_max));
    // It is also idempotent: calling it again must not lower the limit.
    const agentenv::core::Expected<uint64_t, std::string> again =
        setup::RaiseFileDescriptorLimit();
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_TRUE(again.value() >= limit.value());
}

int main() { return microtest::RunAll(); }
