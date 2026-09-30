// SPDX-License-Identifier: MIT
// Rust: src/sandbox/process.rs — `ProcessOpts`, `ProcessOutput`, `Executor`.
#ifndef AGENTENV_SANDBOX_PROCESS_H_
#define AGENTENV_SANDBOX_PROCESS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace sandbox {

/// Rust struct `ProcessOpts`.
struct ProcessOpts {
    /// Rust `envs: HashMap<String, String>`. A map, not a list of `K=V`
    /// strings: a later duplicate key must replace the earlier one rather
    /// than both reaching the process.
    std::map<std::string, std::string> envs;
    /// Rust `cwd: Option<String>`. Unset means "inherit the envd default",
    /// which is distinct from an empty string.
    core::Optional<std::string> cwd;
    /// Rust `timeout: Option<Duration>`. Unset means no timeout.
    core::Optional<int64_t> timeout_ms;

    /// Rust `ProcessOpts::with_envs`.
    ProcessOpts& WithEnvs(const std::map<std::string, std::string>& envs);
    /// Rust `ProcessOpts::with_cwd`.
    ProcessOpts& WithCwd(const std::string& cwd);
    /// Rust `ProcessOpts::with_timeout`.
    ProcessOpts& WithTimeoutMs(int64_t timeout_ms);
};

/// Rust struct `ProcessOutput`.
struct ProcessOutput {
    /// Rust fields `stdout` / `stderr`, renamed because both are macros in the
    /// C standard library and cannot be used as member names.
    std::string stdout_data;
    std::string stderr_data;
    int32_t     exit_code = 0;
};

/// Rust struct `ProcessHandle`.
class ProcessHandle {
 public:
    virtual ~ProcessHandle() {}
    virtual int64_t Pid() const = 0;
    virtual core::Expected<ProcessOutput, std::string> Wait() = 0;
    virtual core::Expected<core::Unit, std::string> Kill(int signal) = 0;
};

/// Rust `Executor` — the process-execution surface the build pipeline uses.
///
/// Upstream this dispatches into the guest over the envd gRPC channel. There is
/// no in-guest transport here, so `MakeLocalExecutor` runs on the host; the
/// interface is the same so the layers above it port unchanged.
class Executor {
 public:
    virtual ~Executor() {}

    /// Rust `Executor::run_command_with_opts`. The command and its arguments
    /// stay separate from the options, so no caller has to splice an argv.
    virtual core::Expected<ProcessOutput, std::string> RunCommandWithOpts(
        const std::string& cmd, const std::vector<std::string>& args,
        const ProcessOpts& opts) = 0;

    /// Rust `Executor::create_dir_all`.
    ///
    /// Goes through envd's filesystem service rather than exec'ing `mkdir`, so
    /// it works in images that ship no userland (scratch, distroless). An
    /// already-existing directory is not an error.
    virtual core::Expected<core::Unit, std::string> CreateDirAll(const std::string& path) = 0;

    /// Rust `Executor::run_command` — a non-virtual convenience over
    /// `RunCommandWithOpts` with default options, matching the Rust default
    /// method.
    core::Expected<ProcessOutput, std::string> RunCommand(
        const std::string& cmd, const std::vector<std::string>& args);
};

/// A concrete host-local Executor backed by fork/exec + pipes + waitpid.
std::unique_ptr<Executor> MakeLocalExecutor();

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_PROCESS_H_
