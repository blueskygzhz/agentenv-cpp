// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{pool,startup_pack,config}.rs
#include "agentenv/sandbox/firecracker/lifecycle.h"

#include "agentenv/core/fs.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

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

}  // namespace

void WarmStdioPaths(const std::string& work_dir, bool capture_output,
                    core::Optional<std::string>* stdout_path,
                    core::Optional<std::string>* stderr_path) {
    if (!capture_output) {
        // Both unset: the VM inherits the parent's stdio.
        *stdout_path = core::Optional<std::string>();
        *stderr_path = core::Optional<std::string>();
        return;
    }
    *stdout_path =
        core::Optional<std::string>(core::fs::Join(work_dir, "firecracker-stdout.log"));
    *stderr_path =
        core::Optional<std::string>(core::fs::Join(work_dir, "firecracker-stderr.log"));
}

core::Expected<core::Unit, std::string> FirecrackerPoolCleanupResult(
    const std::vector<std::string>& failures) {
    if (failures.empty()) return core::Unit();

    // Every failure, not just the first: pool cleanup touches independent
    // entries and an operator needs to see all of them to know what leaked.
    std::string joined;
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i != 0) joined += " | ";
        joined += failures[i];
    }
    return core::make_unexpected("failed to clean up firecracker pool entries: " + joined);
}

bool LoggingEnabled(const core::Optional<std::string>& log_level) {
    // A whitespace-only value counts as unset, so a blank config entry does
    // not create an empty log file.
    return log_level.has_value() && !Trim(*log_level).empty();
}

bool StartupPackRecordingEnabled(bool feature_enabled, bool repository_backend_is_oss) {
    // A POSIX-backed snapshot resolves its memory layers to plain repository
    // file paths, so there is no small-request remote chain for the pack to
    // absorb; recording would cost a throwaway VM for nothing.
    return feature_enabled && repository_backend_is_oss;
}

core::Expected<std::string, std::string> CreateFirecrackerWorkDir(
    const core::Optional<std::string>& parent) {
    if (!parent.has_value() || parent->empty()) {
        return core::fs::CreateTempDir("agentenv-fc-");
    }

    // The parent may not exist yet (a configured pool directory on a fresh
    // node), so it is created before the work directory goes inside it.
    const core::Expected<core::Unit, std::string> created = core::fs::CreateDirAll(*parent);
    if (!created.ok()) {
        return core::make_unexpected("create firecracker work_dir " + *parent + ": " +
                                     created.error());
    }
    return core::fs::CreateTempDirIn(*parent, "agentenv-fc-");
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
