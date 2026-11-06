// SPDX-License-Identifier: MIT
// Rust: src/sandbox/runtime_versions.rs — `probe_runtime_versions`.
//
// The parsers this drives already live in `snapshot/version.h`; what was
// missing is the half that actually runs the commands. Without it nothing in
// production called those parsers, so a snapshot recorded no runtime versions
// at all — and the versions are what a restore checks before resuming a VM
// against a kernel or a VMM it was not captured on.
#ifndef AGENTENV_SANDBOX_RUNTIME_VERSIONS_H_
#define AGENTENV_SANDBOX_RUNTIME_VERSIONS_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/process.h"
#include "agentenv/snapshot/version.h"

namespace agentenv {
namespace sandbox {

/// Runs one host command and returns its output. Injected so the probe logic
/// is testable without a firecracker binary on the machine.
///
/// Separate from `Executor`: the VMM version is asked of a binary **on the
/// host**, while the kernel and envd versions are asked **inside the sandbox**.
/// Collapsing the two would run `firecracker --version` in the guest, where it
/// does not exist.
typedef std::function<core::Expected<ProcessOutput, std::string>(
    const std::string& binary, const std::vector<std::string>& args, int64_t timeout_ms)>
    HostCommandRunner;

/// The production runner: fork/exec on the host.
HostCommandRunner LocalHostCommandRunner();

/// Rust `probe_vmm_version`'s 5 second `tokio::time::timeout`.
///
/// A VMM that hangs on `--version` must not hang the snapshot: this probe is
/// optional (its failure degrades to `"unknown"`), so waiting indefinitely
/// would turn a cosmetic gap into a stuck capture.
extern const int64_t kVmmVersionProbeTimeoutMs;

/// Rust `probe_envd_version` — `sh -lc "envd --version 2>&1 || envd -version 2>&1"`.
///
/// The `||` fallback is upstream's: the two spellings are both in use across
/// envd releases. `2>&1` on each, because some versions print to stderr and
/// the parser is given one combined stream.
core::Expected<std::string, std::string> ProbeEnvdVersion(Executor& sandbox);

/// Rust `probe_kernel_version` — `uname -r`.
core::Expected<std::string, std::string> ProbeKernelVersion(Executor& sandbox);

/// Rust `probe_vmm_version` — `<vmm_binary> --version` on the host.
///
/// stdout and stderr are combined the way upstream does: whichever is
/// non-empty, or both joined when both are. Firecracker has printed its
/// version to either over its history, and reading only one would parse an
/// empty string.
core::Expected<std::string, std::string> ProbeVmmVersion(const std::string& vmm_binary,
                                                         const HostCommandRunner& runner);

/// Rust `probe_runtime_versions`.
///
/// **Only the envd probe is fatal.** The kernel and VMM versions fall back to
/// `"unknown"` with a warning, because they are recorded for diagnostics,
/// while the envd version gates whether a restored sandbox can talk to its
/// guest agent at all — recording `"unknown"` there would let a resume proceed
/// against an envd it was never captured against.
///
/// Rust runs the three concurrently with `tokio::join!`. They are sequential
/// here: there is no async runtime, and three short probes do not justify
/// threads whose only effect would be to reorder the log lines.
core::Expected<snapshot::SnapshotRuntimeVersions, std::string> ProbeRuntimeVersions(
    Executor& sandbox, const std::string& vmm_binary, const std::string& tools_drive_version,
    const HostCommandRunner& runner);

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_RUNTIME_VERSIONS_H_
