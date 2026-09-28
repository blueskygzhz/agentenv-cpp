// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/version.rs
#include "agentenv/snapshot/version.h"

#include <cctype>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace snapshot {

const char* const kUnknownVersion = "unknown";

namespace {

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

/// Rust's split predicate: everything that is not alphanumeric, `.`, `-` or
/// `_` is a separator, so `Firecracker v1.8.0` yields `Firecracker` and
/// `v1.8.0`.
bool IsVersionTokenByte(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x80) return false;  // `is_ascii_alphanumeric` is ASCII-only
    return std::isalnum(byte) != 0 || c == '.' || c == '-' || c == '_';
}

std::string OptionalString(const core::JsonObject& fields, const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return std::string();
    }
    return it->second.as_string();
}

core::Expected<std::string, std::string> RequireString(const core::JsonObject& fields,
                                                       const std::string& field) {
    const core::JsonObject::const_iterator it = fields.find(field);
    if (it == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }
    if (it->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("field `") + field + "` must be a string");
    }
    return it->second.as_string();
}

}  // namespace

SnapshotRuntimeVersions SnapshotRuntimeVersions::New(const std::string& kernel_version,
                                                     const std::string& firecracker_version,
                                                     const std::string& envd_version,
                                                     const std::string& tools_drive_version) {
    SnapshotRuntimeVersions versions;
    versions.kernel_version = kernel_version;
    versions.firecracker_version = firecracker_version;
    versions.envd_version = envd_version;
    versions.tools_drive_version = tools_drive_version;
    return versions;
}

core::Json SnapshotRuntimeVersions::ToJson() const {
    core::JsonObject object;
    object["kernel_version"] = core::Json(kernel_version);
    object["firecracker_version"] = core::Json(firecracker_version);
    object["envd_version"] = core::Json(envd_version);
    object["tools_drive_version"] = core::Json(tools_drive_version);
    return core::Json(object);
}

core::Expected<SnapshotRuntimeVersions, std::string> SnapshotRuntimeVersions::FromJson(
    const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("runtime versions must be an object"));
    }
    const core::JsonObject& fields = json.as_object();
    SnapshotRuntimeVersions versions;

    const core::Expected<std::string, std::string> kernel =
        RequireString(fields, "kernel_version");
    if (!kernel.ok()) return core::make_unexpected(kernel.error());
    versions.kernel_version = kernel.value();

    const core::Expected<std::string, std::string> firecracker =
        RequireString(fields, "firecracker_version");
    if (!firecracker.ok()) return core::make_unexpected(firecracker.error());
    versions.firecracker_version = firecracker.value();

    const core::Expected<std::string, std::string> envd =
        RequireString(fields, "envd_version");
    if (!envd.ok()) return core::make_unexpected(envd.error());
    versions.envd_version = envd.value();

    // `#[serde(default)]`: a record written before the tools drive existed.
    versions.tools_drive_version = OptionalString(fields, "tools_drive_version");

    return versions;
}

bool SnapshotRuntimeVersions::operator==(const SnapshotRuntimeVersions& o) const {
    return kernel_version == o.kernel_version &&
           firecracker_version == o.firecracker_version && envd_version == o.envd_version &&
           tools_drive_version == o.tools_drive_version;
}

core::Optional<std::string> NormalizeVersionToken(const std::string& token) {
    std::string candidate = token;
    if (!candidate.empty() && candidate[0] == 'v') candidate = candidate.substr(1);

    if (candidate.empty()) return core::Optional<std::string>();
    if (std::isdigit(static_cast<unsigned char>(candidate[0])) == 0) {
        return core::Optional<std::string>();
    }
    // Requiring a dot is what keeps a bare word like `release` or a build
    // number like `2024` from being mistaken for a version.
    if (candidate.find('.') == std::string::npos) return core::Optional<std::string>();

    return candidate;
}

core::Expected<std::string, std::string> ParseRuntimeComponentVersion(
    const std::string& output, const std::string& component) {
    std::size_t i = 0;
    while (i < output.size()) {
        while (i < output.size() && !IsVersionTokenByte(output[i])) ++i;
        const std::size_t begin = i;
        while (i < output.size() && IsVersionTokenByte(output[i])) ++i;
        if (i == begin) continue;

        const core::Optional<std::string> version =
            NormalizeVersionToken(output.substr(begin, i - begin));
        if (version.has_value()) return *version;
    }

    return core::make_unexpected(std::string("failed to parse ") + component +
                                 " version from output: " + Trim(output));
}

core::Expected<std::string, std::string> ParseEnvdVersion(const std::string& output) {
    return ParseRuntimeComponentVersion(output, "envd");
}

core::Expected<std::string, std::string> ParseKernelVersion(const std::string& output) {
    // A kernel release such as `6.1.12-agentenv` is taken whole; running it
    // through the token scanner would keep the suffix anyway, but an empty
    // reading has to be an error rather than a blank recorded version.
    const std::string version = Trim(output);
    if (version.empty()) {
        return core::make_unexpected(std::string("kernel version output is empty"));
    }
    return version;
}

core::Expected<std::string, std::string> ParseFirecrackerVersion(const std::string& output) {
    return ParseRuntimeComponentVersion(output, "firecracker");
}

std::string VersionOrUnknown(const std::string& component,
                             const core::Expected<std::string, std::string>& result) {
    if (result.ok()) return result.value();
    AGENTENV_WARN("failed to probe runtime version; falling back to unknown component=",
                  component, " error=", result.error());
    return kUnknownVersion;
}

}  // namespace snapshot
}  // namespace agentenv
