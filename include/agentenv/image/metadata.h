// SPDX-License-Identifier: MIT
// Rust: src/image/metadata.rs
//
// What a resolved image tells the sandbox about how to run it.
//
// The field set here is a wire contract: `ImageBaseContext` is forwarded to
// envd's `InitPostRequest` and `ImageResolutionMetadata` is what gets written
// into the cache's metadata sidecar, so a missing or renamed field is not a
// local simplification -- it changes what the guest is started with, or makes
// cache files written by one side unreadable by the other.
#ifndef AGENTENV_IMAGE_METADATA_H_
#define AGENTENV_IMAGE_METADATA_H_

#include <map>
#include <string>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace image {

/// Rust struct `ImageBaseContext`.
struct ImageBaseContext {
    /// Rust `env_vars: HashMap<String, String>`. Already split into key/value
    /// here rather than kept as a `KEY=VALUE` list, because duplicate keys
    /// must collapse last-wins exactly once -- doing it later, twice, in two
    /// places, is how the two copies disagree.
    std::map<std::string, std::string> env_vars;
    /// Rust `workdir: Option<String>`. Absent and empty are distinct: absent
    /// means "use the image's default", empty would mean "chdir to ''".
    core::Optional<std::string> workdir;
    /// Rust `user: Option<String>`. The OCI `User` field forwarded verbatim to
    /// envd. Accepted shapes: "username", "uid", "user:group", "uid:gid", and
    /// "" (root) -- which is why an explicitly empty string cannot be folded
    /// into absent.
    core::Optional<std::string> user;
    std::vector<std::string> exposed_ports;
    /// Rust `entrypoint: Option<Vec<String>>` — absent is *not* an empty
    /// vector. OCI inheritance treats an explicit empty array as "clear the
    /// inherited entrypoint" and an absent key as "keep it"; a plain vector
    /// cannot express the difference and would silently clear it.
    core::Optional<std::vector<std::string> > entrypoint;
    /// Rust `cmd: Option<Vec<String>>` — same distinction as `entrypoint`.
    core::Optional<std::vector<std::string> > cmd;
    std::vector<std::string> volumes;
    std::map<std::string, std::string> labels;

    bool operator==(const ImageBaseContext& other) const;
    bool operator!=(const ImageBaseContext& other) const { return !(*this == other); }
};

/// Rust `ImageBaseContext::new` — normalizes `workdir` and `user` on the way
/// in.
///
/// A constructor function rather than a real constructor so the normalization
/// cannot be bypassed by aggregate initialization, which is how the struct is
/// otherwise built.
ImageBaseContext MakeImageBaseContext(const std::map<std::string, std::string>& env_vars,
                                      const core::Optional<std::string>& workdir,
                                      const core::Optional<std::string>& user,
                                      const std::vector<std::string>& exposed_ports,
                                      const core::Optional<std::vector<std::string> >& entrypoint,
                                      const core::Optional<std::vector<std::string> >& cmd,
                                      const std::vector<std::string>& volumes,
                                      const std::map<std::string, std::string>& labels);

/// Rust `normalize_optional_string` — trim, then treat the result as absent
/// when empty.
///
/// A whitespace-only `WorkingDir` in an image config is the same mistake as
/// omitting it, and passing `"  "` through would make the guest chdir into a
/// directory named after two spaces.
core::Optional<std::string> NormalizeOptionalString(const core::Optional<std::string>& value);

/// Rust `env_vars_from_entries`.
///
/// Docker-compatible last-wins for duplicate keys. An entry with no `=` is
/// skipped, and so is one with an **empty** key: `"=value"` would otherwise
/// insert a variable with no name, which `execve` rejects for the whole
/// environment rather than just that entry.
std::map<std::string, std::string> EnvVarsFromEntries(const std::vector<std::string>& entries);

/// Rust struct `ImageResolutionMetadata`.
struct ImageResolutionMetadata {
    ImageBaseContext base_context;
    /// Rust `raw_config: Option<serde_json::Value>` — the source image config
    /// JSON, preserved byte-for-byte for pass-through to consumers such as
    /// MMDS, with no field-level interpretation.
    ///
    /// Kept as an optional whole document on purpose: anything this port does
    /// not model is still forwarded, so a consumer reading a field AgentENV
    /// never parsed keeps working.
    core::Optional<core::Json> raw_config;

    bool operator==(const ImageResolutionMetadata& other) const;
    bool operator!=(const ImageResolutionMetadata& other) const { return !(*this == other); }
};

/// Serializes to the shape serde produces for `ImageBaseContext`, honouring
/// every `skip_serializing_if`.
///
/// The omissions are load-bearing: a serialized `"entrypoint": null` is not
/// the same document as one with no `entrypoint` key, and the sidecar is read
/// back by `#[serde(default)]` fields that distinguish them.
core::Json ImageBaseContextToJson(const ImageBaseContext& context);
core::Json ImageResolutionMetadataToJson(const ImageResolutionMetadata& metadata);

/// Parses the same shapes back. A missing key keeps the default, matching
/// `#[serde(default)]`.
core::Expected<ImageBaseContext, std::string> ParseImageBaseContext(const core::Json& value);
core::Expected<ImageResolutionMetadata, std::string> ParseImageResolutionMetadata(
    const core::Json& value);

}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_METADATA_H_
