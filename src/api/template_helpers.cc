// SPDX-License-Identifier: MIT
// Rust: src/api/impls/template_helpers.rs
#include "agentenv/api/template_helpers.h"

#include "agentenv/cfg.h"

namespace agentenv {
namespace api {

// ---------------------------------------------------------------------------
// TemplateBuildStartBaseSource
// ---------------------------------------------------------------------------

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeDefaultImage() {
    return TemplateBuildStartBaseSource();
}

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeImage(const std::string& image) {
    TemplateBuildStartBaseSource source;
    source.kind_  = Image;
    source.image_ = image;
    return source;
}

TemplateBuildStartBaseSource TemplateBuildStartBaseSource::MakeTemplate(
    const snapshot::SnapshotAlias& alias) {
    TemplateBuildStartBaseSource source;
    source.kind_  = Template;
    source.alias_ = core::Optional<snapshot::SnapshotAlias>(alias);
    return source;
}

bool TemplateBuildStartBaseSource::operator==(const TemplateBuildStartBaseSource& o) const {
    if (kind_ != o.kind_) return false;
    switch (kind_) {
        case DefaultImage:
            return true;
        case Image:
            return image_ == o.image_;
        case Template:
            return alias_.has_value() && o.alias_.has_value() && *alias_ == *o.alias_;
    }
    return false;
}

namespace {

/// Rust `str::trim`.
std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                    value[begin] == '\n' || value[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string ToUpperAscii(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = static_cast<char>(out[i] - 'a' + 'A');
    }
    return out;
}

/// Rust `.as_deref().map(str::trim).filter(|v| !v.is_empty())` — an absent
/// field and a whitespace-only one are treated the same.
core::Optional<std::string> TrimmedNonEmpty(const core::Optional<std::string>& value) {
    if (!value.has_value()) return core::Optional<std::string>();
    const std::string trimmed = Trim(*value);
    if (trimmed.empty()) return core::Optional<std::string>();
    return core::Optional<std::string>(trimmed);
}

const std::vector<std::string>& StepArgs(const TemplateStepRequest& step) {
    static const std::vector<std::string> kEmpty;
    return step.args.has_value() ? *step.args : kEmpty;
}

/// Rust's `args.first().filter(|v| !v.trim().is_empty())` — the first argument,
/// but only if it carries something.
core::Expected<std::string, ApiError> FirstNonBlankArg(const TemplateStepRequest& step,
                                                       const std::string& message) {
    const std::vector<std::string>& args = StepArgs(step);
    if (args.empty() || Trim(args[0]).empty()) {
        return core::make_unexpected(ApiError::Make(400, message));
    }
    // Note: the raw value is used, not the trimmed one — Rust only *tests* the
    // trimmed form and then clones the original.
    return args[0];
}

/// Rust's key/value loop over `args.chunks(2)`, shared by ENV/ARG and LABEL.
/// `min_args` differs: ENV/ARG reject an empty list via `args.is_empty()`,
/// LABEL via `args.len() < 2`, which amount to the same thing for an even
/// length, so one parameter covers both.
core::Expected<core::Unit, ApiError> ApplyPairs(tpl::TemplateBuildSpec* spec,
                                                const TemplateStepRequest& step, bool as_label,
                                                const std::string& pair_message,
                                                const std::string& key_message) {
    const std::vector<std::string>& args = StepArgs(step);
    if (args.empty() || args.size() % 2 != 0) {
        return core::make_unexpected(ApiError::Make(400, pair_message));
    }
    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        const std::string key = Trim(args[i]);
        if (key.empty()) {
            return core::make_unexpected(ApiError::Make(400, key_message));
        }
        if (as_label) {
            spec->Label(key, args[i + 1]);
        } else {
            spec->Env(key, args[i + 1]);
        }
    }
    return core::Unit();
}

}  // namespace

// ---------------------------------------------------------------------------
// Step translation
// ---------------------------------------------------------------------------

core::Expected<core::Unit, ApiError> ApplyE2bTemplateStep(tpl::TemplateBuildSpec* spec,
                                                          const TemplateStepRequest& step) {
    // A blank hash means "no upload"; a real one means the client staged files
    // for a COPY, which would silently not happen if accepted here.
    if (step.files_hash.has_value() && !Trim(*step.files_hash).empty()) {
        return core::make_unexpected(ApiError::Make(
            400, "template build filesHash/COPY support is not implemented yet"));
    }

    const std::string type = ToUpperAscii(step.type);

    if (type == "RUN") {
        core::Expected<std::string, ApiError> cmd =
            FirstNonBlankArg(step, "RUN template step requires a command argument");
        if (!cmd.ok()) return core::make_unexpected(cmd.error());
        spec->Run(cmd.value());
        return core::Unit();
    }
    if (type == "ENV" || type == "ARG") {
        // The message quotes the *original* spelling, not the upper-cased one.
        return ApplyPairs(spec, step, false,
                          step.type + " template step requires key/value arguments",
                          step.type + " template step requires a non-empty key");
    }
    if (type == "WORKDIR") {
        core::Expected<std::string, ApiError> path =
            FirstNonBlankArg(step, "WORKDIR template step requires a path argument");
        if (!path.ok()) return core::make_unexpected(path.error());
        spec->Workdir(path.value());
        return core::Unit();
    }
    if (type == "USER") {
        core::Expected<std::string, ApiError> value =
            FirstNonBlankArg(step, "USER step requires a value");
        if (!value.ok()) return core::make_unexpected(value.error());
        spec->User(value.value());
        return core::Unit();
    }
    if (type == "EXPOSE") {
        core::Expected<std::string, ApiError> port =
            FirstNonBlankArg(step, "EXPOSE step requires a port argument");
        if (!port.ok()) return core::make_unexpected(port.error());
        spec->ExposedPort(port.value());
        return core::Unit();
    }
    if (type == "VOLUME") {
        core::Expected<std::string, ApiError> path =
            FirstNonBlankArg(step, "VOLUME step requires a path argument");
        if (!path.ok()) return core::make_unexpected(path.error());
        spec->Volume(path.value());
        return core::Unit();
    }
    if (type == "LABEL") {
        return ApplyPairs(spec, step, true, "LABEL step requires key/value arguments",
                          "LABEL step requires a non-empty key");
    }
    if (type == "COPY" || type == "ADD") {
        return core::make_unexpected(
            ApiError::Make(400, step.type + " template steps are not supported yet"));
    }
    // Rust interpolates the *raw* step type here, not the upper-cased one.
    return core::make_unexpected(
        ApiError::Make(400, "template step type " + step.type + " is not supported"));
}

// ---------------------------------------------------------------------------
// Request translation
// ---------------------------------------------------------------------------

core::Expected<sandbox::SandboxResources, ApiError> ResolveTemplateResources(
    const TemplateBuildRequestV3& body) {
    const cfg::AppConfig* config = cfg::ConfigManager::GlobalConfig();

    // Rust reads the global config unconditionally (it panics if absent). Here
    // a missing global falls back to the same literals `MachineConfig`
    // declares, so a caller that never initialised the config still gets the
    // documented defaults rather than a crash.
    uint32_t cpu_count  = config != NULL ? config->machine.vcpu_count : 2;
    uint32_t memory_mib = config != NULL ? config->machine.mem_size_mib : 1024;
    if (body.cpu_count.has_value()) cpu_count = *body.cpu_count;
    if (body.memory_mb.has_value()) memory_mib = *body.memory_mb;

    if (cpu_count == 0 || memory_mib == 0) {
        return core::make_unexpected(
            ApiError::Make(400, "cpuCount and memoryMB must be greater than 0"));
    }

    sandbox::SandboxResources resources;
    resources.cpu_count  = cpu_count;
    resources.memory_mib = memory_mib;
    // A template build sizes its disk from the image, not from the request.
    resources.disk_size_mib = 0;
    return resources;
}

core::Expected<snapshot::SnapshotRecord, ApiError> TemplateBuildRecordFromV3Request(
    const TemplateBuildRequestV3& body, const core::SnapshotId& id, const std::string& alias) {
    // A `name:tag` reference would silently build the untagged template.
    if (alias.find(':') != std::string::npos) {
        return core::make_unexpected(
            ApiError::Make(400, "template name tags are not supported yet"));
    }
    if (body.tags.has_value() && !body.tags->empty()) {
        return core::make_unexpected(ApiError::Make(400, "template tags are not supported yet"));
    }

    core::Expected<snapshot::SnapshotAlias, std::string> parsed =
        snapshot::SnapshotAlias::Parse(alias);
    if (!parsed.ok()) {
        return core::make_unexpected(ApiError::Make(400, parsed.error()));
    }

    core::Expected<sandbox::SandboxResources, ApiError> resources = ResolveTemplateResources(body);
    if (!resources.ok()) return core::make_unexpected(resources.error());

    return snapshot::SnapshotRecord::TemplateWaiting(
        id, core::Optional<snapshot::SnapshotAlias>(parsed.value()), resources.value());
}

core::Expected<tpl::TemplateBuildSpec, ApiError> TemplateBuildSpecFromStartRequest(
    const TemplateBuildStartRequest& body, const core::Optional<snapshot::SnapshotAlias>& alias,
    const sandbox::SandboxResources& resources) {
    tpl::TemplateBuildSpec spec;
    spec.Resources(resources.cpu_count, resources.memory_mib);
    if (alias.has_value()) spec.Alias(alias->ToString());
    if (body.start_cmd.has_value()) spec.StartCmd(*body.start_cmd);
    if (body.ready_cmd.has_value()) spec.ReadyCmd(*body.ready_cmd);

    if (body.steps.has_value()) {
        const std::vector<TemplateStepRequest>& steps = *body.steps;
        for (std::size_t index = 0; index < steps.size(); ++index) {
            // One client step can expand into several internal steps, so the
            // range it produced is stamped with the client's 1-based number.
            // Without this, a multi-pair ENV would shift every later step's
            // reported position.
            const std::size_t appended_from = spec.StepCount();
            core::Expected<core::Unit, ApiError> applied =
                ApplyE2bTemplateStep(&spec, steps[index]);
            if (!applied.ok()) return core::make_unexpected(applied.error());
            spec.StampSourceSteps(appended_from, index + 1);
        }
    }

    return spec;
}

core::Expected<TemplateBuildStartBaseSource, ApiError> TemplateBuildStartBaseSourceFrom(
    const TemplateBuildStartRequest& body) {
    const core::Optional<std::string> from_image    = TrimmedNonEmpty(body.from_image);
    const core::Optional<std::string> from_template = TrimmedNonEmpty(body.from_template);

    if (from_image.has_value() && from_template.has_value()) {
        return core::make_unexpected(
            ApiError::Make(400, "cannot specify both fromImage and fromTemplate"));
    }
    if (from_image.has_value()) {
        return TemplateBuildStartBaseSource::MakeImage(*from_image);
    }
    if (from_template.has_value()) {
        core::Expected<snapshot::SnapshotAlias, std::string> parsed =
            snapshot::SnapshotAlias::Parse(*from_template);
        if (!parsed.ok()) {
            return core::make_unexpected(ApiError::Make(400, parsed.error()));
        }
        return TemplateBuildStartBaseSource::MakeTemplate(parsed.value());
    }
    return TemplateBuildStartBaseSource::MakeDefaultImage();
}

}  // namespace api
}  // namespace agentenv
