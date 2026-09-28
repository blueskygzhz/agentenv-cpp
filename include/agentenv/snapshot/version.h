// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/version.rs
//
// The runtime versions recorded with a snapshot, and the parsers that pull
// them out of `--version` output. The probes themselves run commands inside a
// sandbox and are async; only the value type and the parsing live here.
#ifndef AGENTENV_SNAPSHOT_VERSION_H_
#define AGENTENV_SNAPSHOT_VERSION_H_

#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace snapshot {

/// Rust `struct SnapshotRuntimeVersions`.
struct SnapshotRuntimeVersions {
    std::string kernel_version;
    std::string firecracker_version;
    std::string envd_version;
    /// Rust `#[serde(default)]` — older records predate the field.
    std::string tools_drive_version;

    /// Rust `SnapshotRuntimeVersions::new`.
    static SnapshotRuntimeVersions New(const std::string& kernel_version,
                                       const std::string& firecracker_version,
                                       const std::string& envd_version,
                                       const std::string& tools_drive_version);

    core::Json ToJson() const;
    static core::Expected<SnapshotRuntimeVersions, std::string> FromJson(
        const core::Json& json);

    bool operator==(const SnapshotRuntimeVersions& o) const;
    bool operator!=(const SnapshotRuntimeVersions& o) const { return !(*this == o); }
};

/// Rust `unknown_version`.
extern const char* const kUnknownVersion;

/// Rust `normalize_version_token` — empty unless the token looks like a
/// version: starts with a digit (after an optional `v`) and contains a dot.
core::Optional<std::string> NormalizeVersionToken(const std::string& token);

/// Rust `parse_runtime_component_version` — scans `--version` output for the
/// first token that looks like a version, so a banner or a prefix word does
/// not have to be stripped by the caller.
core::Expected<std::string, std::string> ParseRuntimeComponentVersion(
    const std::string& output, const std::string& component);

/// Rust `parse_envd_version`.
core::Expected<std::string, std::string> ParseEnvdVersion(const std::string& output);

/// Rust `parse_kernel_version` — `uname -r` output is taken whole, since a
/// kernel release is not a plain SemVer string.
core::Expected<std::string, std::string> ParseKernelVersion(const std::string& output);

/// Rust `parse_firecracker_version`.
core::Expected<std::string, std::string> ParseFirecrackerVersion(const std::string& output);

/// Rust `version_or_unknown` — an optional probe that fails must not fail the
/// snapshot, so the version degrades to "unknown".
std::string VersionOrUnknown(const std::string& component,
                             const core::Expected<std::string, std::string>& result);

}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_VERSION_H_
