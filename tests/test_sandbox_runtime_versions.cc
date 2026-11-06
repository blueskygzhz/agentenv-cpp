// SPDX-License-Identifier: MIT
// Rust: src/sandbox/runtime_versions.rs — `probe_runtime_versions`.
//
// The parsers are covered elsewhere; what is pinned here is the orchestration,
// because that is where the consequential choices are: which probes may fail
// quietly, which may not, and what command each actually runs. A test that
// only checked the happy path would pass against a version of this that ran
// `firecracker --version` inside the guest.
#include "agentenv/sandbox/runtime_versions.h"

#include <map>
#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;           // NOLINT
using namespace agentenv::sandbox;  // NOLINT

namespace {

/// Answers sandbox commands from a script keyed by the full argv, and records
/// what was asked.
///
/// Keyed by argv rather than by the binary alone: the envd probe and the
/// kernel probe both have to be told apart *and* their exact command checked,
/// since `sh -lc "envd --version ..."` is the part that makes the two envd
/// flag spellings work.
struct ScriptedExecutor : public Executor {
    struct Reply {
        ProcessOutput output;
        /// A transport-level failure instead of an exit status.
        std::string error;
    };

    std::map<std::string, Reply> replies;
    std::vector<std::string> asked;

    static std::string Key(const std::string& cmd, const std::vector<std::string>& args) {
        std::string key = cmd;
        for (std::vector<std::string>::size_type i = 0; i < args.size(); ++i) {
            key += "\x1f" + args[i];
        }
        return key;
    }

    void Expect(const std::string& cmd, const std::vector<std::string>& args,
                const std::string& stdout_data, int32_t exit_code = 0) {
        Reply reply;
        reply.output.stdout_data = stdout_data;
        reply.output.exit_code = exit_code;
        replies[Key(cmd, args)] = reply;
    }

    void ExpectFailure(const std::string& cmd, const std::vector<std::string>& args,
                       const std::string& error) {
        Reply reply;
        reply.error = error;
        replies[Key(cmd, args)] = reply;
    }

    core::Expected<ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const ProcessOpts&) override {
        const std::string key = Key(cmd, args);
        asked.push_back(key);
        const std::map<std::string, Reply>::const_iterator at = replies.find(key);
        if (at == replies.end()) {
            return core::make_unexpected("unscripted command: " + key);
        }
        if (!at->second.error.empty()) return core::make_unexpected(at->second.error);
        return at->second.output;
    }

    core::Expected<core::Unit, std::string> CreateDirAll(const std::string&) override {
        return core::make_unexpected(std::string("CreateDirAll not scripted"));
    }
    core::Expected<std::unique_ptr<ProcessHandle>, std::string> StartProcess(
        const std::string&, const std::vector<std::string>&, const ProcessOpts&) override {
        return core::make_unexpected(std::string("StartProcess not scripted"));
    }

    bool Asked(const std::string& cmd, const std::vector<std::string>& args) const {
        const std::string key = Key(cmd, args);
        for (std::vector<std::string>::size_type i = 0; i < asked.size(); ++i) {
            if (asked[i] == key) return true;
        }
        return false;
    }
};

const char* kEnvdCommand = "envd --version 2>&1 || envd -version 2>&1";

std::vector<std::string> EnvdArgs() {
    std::vector<std::string> args;
    args.push_back("-lc");
    args.push_back(kEnvdCommand);
    return args;
}

std::vector<std::string> UnameArgs() {
    std::vector<std::string> args;
    args.push_back("-r");
    return args;
}

/// Records the host commands and answers from a single scripted reply.
struct ScriptedHost {
    ProcessOutput output;
    std::string error;
    std::vector<std::string> binaries;
    std::vector<int64_t> timeouts;
    std::vector<std::string> args_seen;

    ScriptedHost() { output.exit_code = 0; }

    HostCommandRunner Fn() {
        ScriptedHost* self = this;
        return [self](const std::string& binary, const std::vector<std::string>& args,
                      int64_t timeout_ms) -> core::Expected<ProcessOutput, std::string> {
            self->binaries.push_back(binary);
            self->timeouts.push_back(timeout_ms);
            self->args_seen.push_back(args.empty() ? std::string() : args[0]);
            if (!self->error.empty()) return core::make_unexpected(self->error);
            return self->output;
        };
    }
};

/// A sandbox whose probes all succeed, so each test only scripts the
/// deviation it is about.
void ScriptHealthySandbox(ScriptedExecutor* sandbox) {
    sandbox->Expect("sh", EnvdArgs(), "envd version 0.5.15\n");
    sandbox->Expect("uname", UnameArgs(), "6.1.12-agentenv\n");
}

MT_TEST(probe_runtime_versions_happy_path) {
    ScriptedExecutor sandbox;
    ScriptHealthySandbox(&sandbox);
    ScriptedHost host;
    host.output.stdout_data = "Firecracker v1.8.0\n";

    const core::Expected<snapshot::SnapshotRuntimeVersions, std::string> versions =
        ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "tools-2024.1", host.Fn());
    MT_EXPECT_TRUE(versions.ok());
    MT_EXPECT_EQ(versions.value().kernel_version, std::string("6.1.12-agentenv"));
    MT_EXPECT_EQ(versions.value().firecracker_version, std::string("1.8.0"));
    MT_EXPECT_EQ(versions.value().envd_version, std::string("0.5.15"));
    // Passed through, not probed: the sandbox was *given* this one.
    MT_EXPECT_EQ(versions.value().tools_drive_version, std::string("tools-2024.1"));
}

/// The commands themselves, pinned. The envd probe's `||` fallback and its
/// `2>&1` are load-bearing: the two flag spellings are both in use across
/// envd releases, and some print the version to stderr.
MT_TEST(probe_runtime_versions_runs_the_expected_commands) {
    ScriptedExecutor sandbox;
    ScriptHealthySandbox(&sandbox);
    ScriptedHost host;
    host.output.stdout_data = "Firecracker v1.8.0\n";

    MT_EXPECT_TRUE(
        ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "t", host.Fn()).ok());
    MT_EXPECT_TRUE(sandbox.Asked("sh", EnvdArgs()));
    MT_EXPECT_TRUE(sandbox.Asked("uname", UnameArgs()));

    // The VMM is asked on the *host*. Running this inside the guest would ask
    // for a binary that is not there.
    MT_EXPECT_EQ(host.binaries.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(host.binaries[0], std::string("/usr/bin/firecracker"));
    MT_EXPECT_EQ(host.args_seen[0], std::string("--version"));
    // Bounded, because this probe is optional: a VMM that hangs on
    // `--version` must not hang the capture.
    MT_EXPECT_EQ(host.timeouts[0], kVmmVersionProbeTimeoutMs);
    // The sandbox was never asked for firecracker.
    MT_EXPECT_EQ(sandbox.asked.size(), static_cast<std::size_t>(2));
}

/// Kernel and VMM are recorded for diagnostics, so their failure degrades to
/// "unknown" rather than failing the capture.
MT_TEST(probe_runtime_versions_falls_back_to_unknown_for_kernel) {
    ScriptedExecutor sandbox;
    sandbox.Expect("sh", EnvdArgs(), "envd version 0.5.15\n");
    sandbox.Expect("uname", UnameArgs(), "", 1);
    ScriptedHost host;
    host.output.stdout_data = "Firecracker v1.8.0\n";

    const core::Expected<snapshot::SnapshotRuntimeVersions, std::string> versions =
        ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "t", host.Fn());
    MT_EXPECT_TRUE(versions.ok());
    MT_EXPECT_EQ(versions.value().kernel_version, std::string(snapshot::kUnknownVersion));
    // The others are unaffected: one optional probe failing must not blank
    // the readings that did succeed.
    MT_EXPECT_EQ(versions.value().envd_version, std::string("0.5.15"));
    MT_EXPECT_EQ(versions.value().firecracker_version, std::string("1.8.0"));
}

MT_TEST(probe_runtime_versions_falls_back_to_unknown_for_vmm) {
    ScriptedExecutor sandbox;
    ScriptHealthySandbox(&sandbox);
    ScriptedHost host;
    host.error = "no such file or directory";

    const core::Expected<snapshot::SnapshotRuntimeVersions, std::string> versions =
        ProbeRuntimeVersions(sandbox, "/nope/firecracker", "t", host.Fn());
    MT_EXPECT_TRUE(versions.ok());
    MT_EXPECT_EQ(versions.value().firecracker_version,
                 std::string(snapshot::kUnknownVersion));
    MT_EXPECT_EQ(versions.value().kernel_version, std::string("6.1.12-agentenv"));
}

/// Unparsable output degrades too -- it is the same "could not read it"
/// outcome as a non-zero exit, and upstream treats both through
/// `version_or_unknown`.
MT_TEST(probe_runtime_versions_unknown_when_vmm_output_unparsable) {
    ScriptedExecutor sandbox;
    ScriptHealthySandbox(&sandbox);
    ScriptedHost host;
    host.output.stdout_data = "Firecracker release build\n";

    const core::Expected<snapshot::SnapshotRuntimeVersions, std::string> versions =
        ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "t", host.Fn());
    MT_EXPECT_TRUE(versions.ok());
    MT_EXPECT_EQ(versions.value().firecracker_version,
                 std::string(snapshot::kUnknownVersion));
}

/// **envd is the one that must not degrade.** It gates whether a restored
/// sandbox can talk to its guest agent, so recording "unknown" would let a
/// resume proceed against an envd the snapshot was never captured against.
MT_TEST(probe_runtime_versions_fails_when_envd_probe_fails) {
    ScriptedExecutor sandbox;
    sandbox.Expect("sh", EnvdArgs(), "", 127);
    sandbox.Expect("uname", UnameArgs(), "6.1.12-agentenv\n");
    ScriptedHost host;
    host.output.stdout_data = "Firecracker v1.8.0\n";

    const core::Expected<snapshot::SnapshotRuntimeVersions, std::string> versions =
        ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "t", host.Fn());
    MT_EXPECT_TRUE(!versions.ok());
    // The exit code is in the message: a probe that failed says why on one of
    // the two streams, and both are reported.
    MT_EXPECT_TRUE(versions.error().find("127") != std::string::npos);
}

MT_TEST(probe_runtime_versions_fails_when_envd_output_unparsable) {
    ScriptedExecutor sandbox;
    sandbox.Expect("sh", EnvdArgs(), "envd release\n");
    sandbox.Expect("uname", UnameArgs(), "6.1.12-agentenv\n");
    ScriptedHost host;
    host.output.stdout_data = "Firecracker v1.8.0\n";

    MT_EXPECT_TRUE(
        !ProbeRuntimeVersions(sandbox, "/usr/bin/firecracker", "t", host.Fn()).ok());
}

/// A transport failure is distinguished from a non-zero exit, because the
/// first means the command never ran.
MT_TEST(probe_envd_version_reports_transport_failure) {
    ScriptedExecutor sandbox;
    sandbox.ExpectFailure("sh", EnvdArgs(), "channel closed");

    const core::Expected<std::string, std::string> probed = ProbeEnvdVersion(sandbox);
    MT_EXPECT_TRUE(!probed.ok());
    MT_EXPECT_TRUE(probed.error().find("channel closed") != std::string::npos);
    MT_EXPECT_TRUE(probed.error().find("snapshot build sandbox") != std::string::npos);
}

/// A kernel release is taken whole, suffix included -- `6.1.12-agentenv` is
/// the identity of the kernel, and keeping only `6.1.12` would compare equal
/// to a different build.
MT_TEST(probe_kernel_version_keeps_the_release_suffix) {
    ScriptedExecutor sandbox;
    sandbox.Expect("uname", UnameArgs(), "6.1.12-agentenv\n");
    MT_EXPECT_EQ(ProbeKernelVersion(sandbox).value(), std::string("6.1.12-agentenv"));
}

/// Firecracker has printed its version to stderr as well as stdout over its
/// history. Reading only stdout would parse an empty string.
MT_TEST(probe_vmm_version_reads_stderr_when_stdout_empty) {
    ScriptedHost host;
    host.output.stderr_data = "Firecracker v1.8.0\n";
    MT_EXPECT_EQ(ProbeVmmVersion("/usr/bin/firecracker", host.Fn()).value(),
                 std::string("1.8.0"));
}

/// With both streams written, both are kept, so a version on either is found.
MT_TEST(probe_vmm_version_combines_both_streams) {
    ScriptedHost host;
    host.output.stdout_data = "Firecracker\n";
    host.output.stderr_data = "v1.8.0\n";
    MT_EXPECT_EQ(ProbeVmmVersion("/usr/bin/firecracker", host.Fn()).value(),
                 std::string("1.8.0"));
}

/// The failure message carries the combined output, not just the status: a
/// VMM that refuses to run says why on one of the two streams.
MT_TEST(probe_vmm_version_reports_output_on_non_zero_exit) {
    ScriptedHost host;
    host.output.exit_code = 2;
    host.output.stderr_data = "permission denied\n";

    const core::Expected<std::string, std::string> probed =
        ProbeVmmVersion("/usr/bin/firecracker", host.Fn());
    MT_EXPECT_TRUE(!probed.ok());
    MT_EXPECT_TRUE(probed.error().find("permission denied") != std::string::npos);
    MT_EXPECT_TRUE(probed.error().find("status=2") != std::string::npos);
}

}  // namespace

MT_MAIN
