// SPDX-License-Identifier: MIT
// Rust: src/template/build_spec.rs
#include "agentenv/template/build_spec.h"

#include <cctype>
#include <sstream>

namespace agentenv {
namespace tpl {
namespace {

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

TemplateBuildStep MakeStep(TemplateBuildStepKind kind, const std::string& key,
                           const std::string& value) {
    TemplateBuildStep step;
    step.kind = kind;
    step.key = key;
    step.value = value;
    return step;
}

}  // namespace

// ---------------------------------------------------------------------------
// ImageConfigs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TemplateBuildRootfsBase
// ---------------------------------------------------------------------------

TemplateBuildRootfsBase TemplateBuildRootfsBase::Ext4(const std::string& image_path) {
    TemplateBuildRootfsBase base;
    base.kind = Kind::Ext4;
    base.image_path = image_path;
    return base;
}

TemplateBuildRootfsBase TemplateBuildRootfsBase::Overlaybd(const std::string& image_config_path,
                                                           const ImageConfigs& image_configs) {
    TemplateBuildRootfsBase base;
    base.kind = Kind::Overlaybd;
    base.image_config_path = image_config_path;
    base.image_configs = image_configs;
    return base;
}

// ---------------------------------------------------------------------------
// TemplateBuildStep
// ---------------------------------------------------------------------------

TemplateBuildStep TemplateBuildStep::Run(const std::string& cmd) {
    return MakeStep(TemplateBuildStepKind::Run, std::string(), cmd);
}
TemplateBuildStep TemplateBuildStep::Env(const std::string& key, const std::string& value) {
    return MakeStep(TemplateBuildStepKind::Env, key, value);
}
TemplateBuildStep TemplateBuildStep::Workdir(const std::string& path) {
    return MakeStep(TemplateBuildStepKind::Workdir, std::string(), path);
}
TemplateBuildStep TemplateBuildStep::User(const std::string& value) {
    return MakeStep(TemplateBuildStepKind::User, std::string(), value);
}
TemplateBuildStep TemplateBuildStep::ExposedPort(const std::string& port) {
    return MakeStep(TemplateBuildStepKind::ExposedPort, std::string(), port);
}
TemplateBuildStep TemplateBuildStep::Volume(const std::string& path) {
    return MakeStep(TemplateBuildStepKind::Volume, std::string(), path);
}
TemplateBuildStep TemplateBuildStep::Label(const std::string& key, const std::string& value) {
    return MakeStep(TemplateBuildStepKind::Label, key, value);
}

bool TemplateBuildStep::operator==(const TemplateBuildStep& o) const {
    if (kind != o.kind || key != o.key || value != o.value) return false;
    if (source_step.has_value() != o.source_step.has_value()) return false;
    return !source_step.has_value() || *source_step == *o.source_step;
}

std::ostream& operator<<(std::ostream& os, TemplateBuildStepKind kind) {
    switch (kind) {
        case TemplateBuildStepKind::Run:
            return os << "Run";
        case TemplateBuildStepKind::Env:
            return os << "Env";
        case TemplateBuildStepKind::Workdir:
            return os << "Workdir";
        case TemplateBuildStepKind::User:
            return os << "User";
        case TemplateBuildStepKind::ExposedPort:
            return os << "ExposedPort";
        case TemplateBuildStepKind::Volume:
            return os << "Volume";
        case TemplateBuildStepKind::Label:
            return os << "Label";
    }
    return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, const TemplateBuildStep& step) {
    os << step.kind << "{";
    if (!step.key.empty()) os << step.key << "=";
    os << step.value << "}";
    if (step.source_step.has_value()) os << "@" << *step.source_step;
    return os;
}

// ---------------------------------------------------------------------------
// TemplateBuildSpec
// ---------------------------------------------------------------------------

TemplateBuildSpec& TemplateBuildSpec::FromExistingRootfs(const std::string& path) {
    rootfs_base_ = TemplateBuildRootfsBase::Ext4(path);
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::FromOverlaybdConfig(const std::string& image_config_path) {
    return WithResolvedOverlaybdImage(image_config_path, ImageConfigs());
}

TemplateBuildSpec& TemplateBuildSpec::WithResolvedOverlaybdImage(
    const std::string& image_config_path, const ImageConfigs& image_configs) {
    rootfs_base_ = TemplateBuildRootfsBase::Overlaybd(image_config_path, image_configs);
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::Run(const std::string& cmd) {
    steps_.push_back(TemplateBuildStep::Run(cmd));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::Env(const std::string& key, const std::string& value) {
    steps_.push_back(TemplateBuildStep::Env(key, value));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::Workdir(const std::string& path) {
    steps_.push_back(TemplateBuildStep::Workdir(path));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::User(const std::string& value) {
    steps_.push_back(TemplateBuildStep::User(value));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::ExposedPort(const std::string& port) {
    steps_.push_back(TemplateBuildStep::ExposedPort(port));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::Volume(const std::string& path) {
    steps_.push_back(TemplateBuildStep::Volume(path));
    return *this;
}
TemplateBuildSpec& TemplateBuildSpec::Label(const std::string& key, const std::string& value) {
    steps_.push_back(TemplateBuildStep::Label(key, value));
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::Apt(const std::vector<std::string>& packages) {
    // Blank entries are dropped, and an all-blank list appends no step at all.
    std::vector<std::string> kept;
    kept.reserve(packages.size());
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (!Trim(packages[i]).empty()) kept.push_back(packages[i]);
    }
    if (kept.empty()) return *this;

    std::ostringstream joined;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) joined << " ";
        joined << kept[i];
    }

    std::ostringstream cmd;
    cmd << "DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y "
           "--no-install-recommends "
        << joined.str() << " && rm -rf /var/lib/apt/lists/*";
    steps_.push_back(TemplateBuildStep::Run(cmd.str()));
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::Alias(const std::string& alias) {
    alias_ = alias;
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::Resources(uint32_t cpu_count, uint32_t memory_mib) {
    sandbox::SandboxResources resources;
    resources.cpu_count = cpu_count;
    resources.memory_mib = memory_mib;
    resources.disk_size_mib = 0;  // disk size is determined after build.
    resources_ = resources;
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::StartCmd(const std::string& cmd) {
    // Rust: `(!cmd.trim().is_empty()).then_some(cmd)` — note it stores the
    // *untrimmed* command, and a blank one clears any previous value.
    if (Trim(cmd).empty()) {
        start_cmd_ = core::Optional<std::string>();
    } else {
        start_cmd_ = cmd;
    }
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::ReadyCmd(const std::string& cmd) {
    if (Trim(cmd).empty()) {
        ready_cmd_ = core::Optional<std::string>();
    } else {
        ready_cmd_ = cmd;
    }
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::WithStartupShell(const std::string& shell) {
    startup_shell_ = shell;
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::WithBaseContext(const snapshot::CommandContext& context) {
    base_context_ = context;
    return *this;
}

TemplateBuildSpec& TemplateBuildSpec::StampSourceSteps(std::size_t from,
                                                       std::size_t source_step) {
    for (std::size_t i = from; i < steps_.size(); ++i) {
        steps_[i].source_step = source_step;
    }
    return *this;
}

TemplateBuildResult<core::Optional<snapshot::SnapshotAlias> > TemplateBuildSpec::ParsedAlias()
    const {
    if (!alias_.has_value()) return core::Optional<snapshot::SnapshotAlias>();

    const core::Expected<snapshot::SnapshotAlias, std::string> parsed =
        snapshot::SnapshotAlias::Parse(*alias_);
    if (!parsed.ok()) {
        return core::make_unexpected(TemplateBuildError::InvalidInput(parsed.error()));
    }
    return core::Optional<snapshot::SnapshotAlias>(parsed.value());
}

}  // namespace tpl
}  // namespace agentenv
