// SPDX-License-Identifier: MIT
// Rust: src/sandbox/runtime_versions.rs
#include "agentenv/sandbox/runtime_versions.h"

#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace sandbox {

const int64_t kVmmVersionProbeTimeoutMs = 5 * 1000;

namespace {

std::string Trim(const std::string& value) {
    std::string::size_type begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::string::size_type end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string ToDecimal(int64_t value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

/// Rust's `bail!("... exit_code={}, stdout='{}', stderr='{}'")`.
///
/// Both streams are reported, trimmed: a probe that failed usually says why on
/// exactly one of them, and a message carrying only the other is the one that
/// sends someone reading the wrong log.
std::string ProbeFailure(const std::string& what, const ProcessOutput& output) {
    return what + " failed: exit_code=" + ToDecimal(output.exit_code) + ", stdout='" +
           Trim(output.stdout_data) + "', stderr='" + Trim(output.stderr_data) + "'";
}

}  // namespace

HostCommandRunner LocalHostCommandRunner() {
    return [](const std::string& binary, const std::vector<std::string>& args,
              int64_t timeout_ms) -> core::Expected<ProcessOutput, std::string> {
        // A fresh executor per call: this runs a handful of times per capture,
        // and holding one would make the runner stateful for no gain.
        const std::unique_ptr<Executor> executor = MakeLocalExecutor();
        ProcessOpts opts;
        opts.WithTimeoutMs(timeout_ms);
        return executor->RunCommandWithOpts(binary, args, opts);
    };
}

core::Expected<std::string, std::string> ProbeEnvdVersion(Executor& sandbox) {
    std::vector<std::string> args;
    args.push_back("-lc");
    // Upstream's two spellings and their `2>&1`: envd releases differ on both
    // the flag and the stream, and the parser is handed one combined reading.
    args.push_back("envd --version 2>&1 || envd -version 2>&1");

    const core::Expected<ProcessOutput, std::string> output = sandbox.RunCommand("sh", args);
    if (!output.ok()) {
        return core::make_unexpected("run envd version probe in snapshot build sandbox: " +
                                     output.error());
    }
    if (output.value().exit_code != 0) {
        return core::make_unexpected(ProbeFailure("envd version probe", output.value()));
    }
    return snapshot::ParseEnvdVersion(output.value().stdout_data);
}

core::Expected<std::string, std::string> ProbeKernelVersion(Executor& sandbox) {
    std::vector<std::string> args;
    args.push_back("-r");

    const core::Expected<ProcessOutput, std::string> output =
        sandbox.RunCommand("uname", args);
    if (!output.ok()) {
        return core::make_unexpected("run kernel version probe in snapshot build sandbox: " +
                                     output.error());
    }
    if (output.value().exit_code != 0) {
        return core::make_unexpected(ProbeFailure("kernel version probe", output.value()));
    }
    return snapshot::ParseKernelVersion(output.value().stdout_data);
}

core::Expected<std::string, std::string> ProbeVmmVersion(const std::string& vmm_binary,
                                                         const HostCommandRunner& runner) {
    std::vector<std::string> args;
    args.push_back("--version");

    const core::Expected<ProcessOutput, std::string> output =
        runner(vmm_binary, args, kVmmVersionProbeTimeoutMs);
    if (!output.ok()) {
        return core::make_unexpected("run firecracker version probe using " + vmm_binary +
                                     ": " + output.error());
    }

    // Rust's three-way combine. Reading only stdout would parse an empty
    // string on the firecracker builds that print to stderr.
    const std::string stdout_trimmed = Trim(output.value().stdout_data);
    const std::string stderr_trimmed = Trim(output.value().stderr_data);
    std::string combined;
    if (stderr_trimmed.empty()) {
        combined = output.value().stdout_data;
    } else if (stdout_trimmed.empty()) {
        combined = output.value().stderr_data;
    } else {
        combined = stdout_trimmed + "\n" + stderr_trimmed;
    }

    if (output.value().exit_code != 0) {
        return core::make_unexpected("firecracker version probe failed: status=" +
                                     ToDecimal(output.value().exit_code) + ", output='" +
                                     Trim(combined) + "'");
    }
    return snapshot::ParseFirecrackerVersion(combined);
}

core::Expected<snapshot::SnapshotRuntimeVersions, std::string> ProbeRuntimeVersions(
    Executor& sandbox, const std::string& vmm_binary, const std::string& tools_drive_version,
    const HostCommandRunner& runner) {
    // Optional: recorded for diagnostics, so a failure degrades to "unknown"
    // rather than failing the capture.
    const std::string kernel_version =
        snapshot::VersionOrUnknown("kernel", ProbeKernelVersion(sandbox));
    const std::string firecracker_version =
        snapshot::VersionOrUnknown("vmm", ProbeVmmVersion(vmm_binary, runner));

    // Fatal, unlike the other two: this one gates whether a restored sandbox
    // can talk to its guest agent, and "unknown" would let a resume proceed
    // against an envd the snapshot was never captured against.
    const core::Expected<std::string, std::string> envd_version = ProbeEnvdVersion(sandbox);
    if (!envd_version.ok()) return core::make_unexpected(envd_version.error());

    AGENTENV_DEBUG("probed runtime versions kernel_version="
                   << kernel_version << " firecracker_version=" << firecracker_version
                   << " envd_version=" << envd_version.value()
                   << " tools_drive_version=" << tools_drive_version);

    return snapshot::SnapshotRuntimeVersions::New(kernel_version, firecracker_version,
                                                  envd_version.value(), tools_drive_version);
}

}  // namespace sandbox
}  // namespace agentenv
