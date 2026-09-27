// SPDX-License-Identifier: MIT
// Rust: src/sandbox/process.rs — process spawning primitive.
#ifndef AGENTENV_SANDBOX_PROCESS_H_
#define AGENTENV_SANDBOX_PROCESS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {

/// Rust struct `ProcessOpts`.
struct ProcessOpts {
    std::vector<std::string> argv;
    std::vector<std::string> env_vars;
    std::string              cwd;
    int32_t                  timeout_sec = 0;   // 0 == no timeout
    bool                     capture_stdout = true;
    bool                     capture_stderr = true;
};

/// Rust struct `ProcessOutput`.
struct ProcessOutput {
    int32_t     exit_code = 0;
    std::string stdout_data;
    std::string stderr_data;
};

/// Rust struct `ProcessHandle`.
class ProcessHandle {
 public:
    virtual ~ProcessHandle() {}
    virtual int64_t Pid() const = 0;
    virtual core::Expected<ProcessOutput, std::string> Wait() = 0;
    virtual core::Expected<core::Unit, std::string> Kill(int signal) = 0;
};

/// Rust trait `Executor` (kept as an ABC; a real impl uses posix_spawn).
class Executor {
 public:
    virtual ~Executor() {}
    virtual core::Expected<ProcessOutput, std::string> Run(const ProcessOpts& opts) = 0;
};

/// A concrete host-local Executor backed by fork/exec + pipes + waitpid.
///
/// The upstream Rust `Executor` dispatches into the guest VM over the envd gRPC
/// channel; there is no in-guest transport in this skeleton, so the local
/// executor runs the command on the host. It faithfully implements the
/// ProcessOpts/ProcessOutput contract (env, cwd, stdout/stderr capture, timeout,
/// SIGKILL on timeout) so the rest of the sandbox layer can be exercised.
std::unique_ptr<Executor> MakeLocalExecutor();

}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_PROCESS_H_
