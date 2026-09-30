// SPDX-License-Identifier: MIT
// Rust: src/template/step_executor.rs
#include "agentenv/template/step_executor.h"

#include <sstream>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace tpl {

namespace {

std::string StepNumberToString(std::size_t step_no) {
    std::ostringstream out;
    out << step_no;
    return out.str();
}

}  // namespace

std::string ResolveWorkdir(const std::string& current, const std::string& path) {
    const std::string base = current.empty() ? "/" : current;
    // An absolute WORKDIR replaces the current one; a relative one joins it.
    const std::string joined =
        (!path.empty() && path[0] == '/') ? path
                                          : (base == "/" ? "/" + path : base + "/" + path);

    // Lexical normalization only: the directory may not exist yet, so nothing
    // here may consult the filesystem.
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= joined.size()) {
        const std::size_t end = joined.find('/', begin);
        const std::string component =
            end == std::string::npos ? joined.substr(begin) : joined.substr(begin, end - begin);
        // Empty (a doubled or trailing separator) and "." contribute nothing.
        if (!component.empty() && component != ".") {
            if (component == "..") {
                // Popping an empty stack is a no-op: `/..` is `/`, never an
                // escape above the root.
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(component);
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    if (parts.empty()) return "/";
    std::string resolved;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        resolved += "/";
        resolved += parts[i];
    }
    return resolved;
}

core::Expected<core::Unit, TemplateBuildFailure> TemplateStepExecutor::EnsureWorkdir(
    sandbox::Executor* sandbox, const std::string& path, std::size_t step_no) const {
    const core::Expected<core::Unit, std::string> created = sandbox->CreateDirAll(path);
    if (!created.ok()) {
        return core::make_unexpected(TemplateBuildFailure::WithStep(
            "build step failed: WORKDIR " + path, StepNumberToString(step_no)));
    }
    return core::Unit();
}

core::Expected<core::Unit, TemplateBuildFailure> TemplateStepExecutor::RunStep(
    sandbox::Executor* sandbox, const std::string& workdir,
    const std::map<std::string, std::string>& env, const std::string& cmd,
    std::size_t step_no) const {
    sandbox::ProcessOpts opts;
    opts.envs = env;
    opts.cwd  = core::Optional<std::string>(workdir);

    // `-lc` runs a login shell, so the image's profile scripts apply — a RUN
    // step behaves the way the same command would in a Dockerfile.
    std::vector<std::string> args;
    args.push_back("-lc");
    args.push_back(cmd);

    const core::Expected<sandbox::ProcessOutput, std::string> output =
        sandbox->RunCommandWithOpts("/bin/bash", args, opts);
    if (!output.ok()) {
        return core::make_unexpected(TemplateBuildFailure::WithStep(
            "build step failed: RUN " + cmd, StepNumberToString(step_no)));
    }
    if (output.value().exit_code != 0) {
        std::ostringstream message;
        message << "build step failed: RUN " << cmd << ": command exited with status "
                << output.value().exit_code
                << CommandOutputSuffix(output.value().stdout_data, output.value().stderr_data);
        return core::make_unexpected(
            TemplateBuildFailure::WithStep(message.str(), StepNumberToString(step_no)));
    }
    return core::Unit();
}

core::Expected<snapshot::CommandContext, TemplateBuildFailure> TemplateStepExecutor::Execute(
    sandbox::Executor* sandbox, const std::vector<TemplateBuildStep>& steps,
    const snapshot::CommandContext& initial_context) const {
    snapshot::CommandContext context = initial_context;

    AGENTENV_DEBUG("executing template build steps");
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const TemplateBuildStep& step = steps[index];
        // The client-visible number, which is what the e2b SDKs parse.
        const std::size_t step_no =
            step.source_step.has_value() ? *step.source_step : index + 1;

        switch (step.kind) {
            case TemplateBuildStepKind::Env:
                context.WithEnvVar(step.key, step.value);
                break;

            case TemplateBuildStepKind::Workdir: {
                const std::string resolved = ResolveWorkdir(context.workdir, step.value);
                // Create before recording: a workdir that could not be made
                // must fail the build rather than be carried forward.
                const core::Expected<core::Unit, TemplateBuildFailure> ensured =
                    EnsureWorkdir(sandbox, resolved, step_no);
                if (!ensured.ok()) return core::make_unexpected(ensured.error());
                context.WithWorkdir(resolved);
                break;
            }

            case TemplateBuildStepKind::User:
                context.WithUser(core::Optional<std::string>(step.value));
                break;

            case TemplateBuildStepKind::ExposedPort: {
                // Deduplicated: a port already exposed by the base image must
                // not be listed twice.
                std::vector<std::string> ports = context.exposed_ports;
                bool present = false;
                for (std::size_t i = 0; i < ports.size(); ++i) {
                    if (ports[i] == step.value) present = true;
                }
                if (!present) ports.push_back(step.value);
                context.WithExposedPorts(ports);
                break;
            }

            case TemplateBuildStepKind::Volume: {
                std::vector<std::string> volumes = context.volumes;
                bool present = false;
                for (std::size_t i = 0; i < volumes.size(); ++i) {
                    if (volumes[i] == step.value) present = true;
                }
                if (!present) volumes.push_back(step.value);
                context.WithVolumes(volumes);
                break;
            }

            case TemplateBuildStepKind::Label: {
                // A map, so a repeated key replaces rather than accumulates.
                std::map<std::string, std::string> labels = context.labels;
                labels[step.key] = step.value;
                context.WithLabels(labels);
                break;
            }

            case TemplateBuildStepKind::Run: {
                const core::Expected<core::Unit, TemplateBuildFailure> ran = RunStep(
                    sandbox, context.workdir, context.env_vars, step.value, step_no);
                if (!ran.ok()) return core::make_unexpected(ran.error());
                break;
            }
        }
    }
    AGENTENV_DEBUG("template build steps completed");

    return context;
}

}  // namespace tpl
}  // namespace agentenv
