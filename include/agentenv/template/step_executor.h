// SPDX-License-Identifier: MIT
// Rust: src/template/step_executor.rs — applies build steps to a live sandbox.
//
// Most steps only mutate the recorded `CommandContext`; WORKDIR and RUN are
// the two that actually touch the sandbox.
//
// Two details carry real weight:
//
//  * WORKDIR must *create* the directory, not merely record it. Docker's
//    WORKDIR does, and both Dockerfile front-ends (`aenv build` and the e2b
//    SDK's `from_dockerfile`, which injects a default `WORKDIR /home/user`)
//    map onto this step. Creation goes through the executor's filesystem
//    service rather than exec'ing `mkdir`, because minimal images (scratch,
//    distroless, Nix-style) may ship no userland at all.
//
//  * A failure's step number must be the *client-visible* one. The e2b SDKs
//    parse `reason.step` as an integer index into their own recorded stack
//    traces; a non-numeric value crashes the Python SDK's error path before it
//    can surface the real build error. `source_step` carries the client number
//    when a front-end expanded one request step into several internal ones,
//    and the 1-based position is the fallback for front-ends that map 1:1.
#ifndef AGENTENV_TEMPLATE_STEP_EXECUTOR_H_
#define AGENTENV_TEMPLATE_STEP_EXECUTOR_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/sandbox/process.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/build_spec.h"
#include "agentenv/template/errors.h"

namespace agentenv {
namespace tpl {

/// Rust `resolve_workdir` — resolves a WORKDIR value to the absolute,
/// lexically normalized path Docker would record. Relative paths join the
/// current workdir; `.` and `..` are resolved without consulting the
/// filesystem (the directory may not exist yet).
std::string ResolveWorkdir(const std::string& current, const std::string& path);

/// Rust struct `TemplateStepExecutor`.
class TemplateStepExecutor {
 public:
    /// Rust `TemplateStepExecutor::new`.
    TemplateStepExecutor() {}

    /// Rust `TemplateStepExecutor::execute` — folds the steps over
    /// `initial_context` and returns the resulting context.
    ///
    /// The context is threaded through rather than rebuilt, so a later step
    /// sees the environment and workdir the earlier ones established.
    core::Expected<snapshot::CommandContext, TemplateBuildFailure> Execute(
        sandbox::Executor* sandbox, const std::vector<TemplateBuildStep>& steps,
        const snapshot::CommandContext& initial_context) const;

 private:
    /// Rust `TemplateStepExecutor::ensure_workdir`.
    core::Expected<core::Unit, TemplateBuildFailure> EnsureWorkdir(sandbox::Executor* sandbox,
                                                                    const std::string& path,
                                                                    std::size_t step_no) const;

    /// Rust `TemplateStepExecutor::run_step`.
    core::Expected<core::Unit, TemplateBuildFailure> RunStep(
        sandbox::Executor* sandbox, const std::string& workdir,
        const std::map<std::string, std::string>& env, const std::string& cmd,
        std::size_t step_no) const;
};

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_STEP_EXECUTOR_H_
