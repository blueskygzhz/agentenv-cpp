// SPDX-License-Identifier: MIT
// Rust: src/setup/overlaybd.rs
//
// Installs the overlaybd CLI tools from a release tarball and stages their
// default config.
#ifndef AGENTENV_SETUP_OVERLAYBD_H_
#define AGENTENV_SETUP_OVERLAYBD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace setup {
namespace overlaybd {

/// Rust `OVERLAYBD_TOOL_NAMES` — the four binaries the runtime actually calls.
/// An installation missing any of them is treated as absent, not partial.
const std::vector<std::string>& ToolNames();

/// The version `EnsureToolsConverterV1` pins.
extern const char* const kConverterV1Version;
/// The package URL template `EnsureToolsConverterV1` pins.
extern const char* const kConverterV1PackageUrl;

/// Rust `crate::cfg::OverlaybdDependencyConfig`, as this module consumes it.
struct DependencyConfig {
    std::string version;
    /// Legacy field; `package_url` takes precedence when both are set.
    core::Optional<std::string> url;
    core::Optional<std::string> package_url;
};

/// Rust struct `ConfiguredOverlaybdRelease` — a release with its URL template
/// already expanded.
struct ConfiguredRelease {
    std::string tag_name;
    std::string package_url;

    bool operator==(const ConfiguredRelease& o) const {
        return tag_name == o.tag_name && package_url == o.package_url;
    }
    bool operator!=(const ConfiguredRelease& o) const { return !(*this == o); }
};

/// Rust struct `OverlaybdInstalledRelease` — the `tools-release.json` marker
/// that records what is installed, so a re-run can skip the download.
struct InstalledRelease {
    std::string tag_name;
    std::string asset_name;
    /// Rust `Option<String>`, currently always `None`, but part of the
    /// serialized shape so an older/newer marker still round-trips.
    core::Optional<std::string> digest;

    /// Serializes to the same JSON shape `serde_json::to_vec_pretty` produces.
    std::string ToJson() const;
    static core::Expected<InstalledRelease, std::string> ParseJson(const std::string& text);

    bool operator==(const InstalledRelease& o) const;
    bool operator!=(const InstalledRelease& o) const { return !(*this == o); }
};

/// Rust `configured_overlaybd_release` — validates the version and
/// architecture, then expands `{version}` and `{arch}` in the URL template.
core::Expected<ConfiguredRelease, std::string> ConfiguredReleaseFor(
    const DependencyConfig& config, const std::string& arch);

/// Rust `desired_overlaybd_installed_release`.
InstalledRelease DesiredInstalledRelease(const ConfiguredRelease& release,
                                         const std::string& asset_name);

/// Rust `read_overlaybd_installed_release` — a missing marker is an empty
/// optional, not an error.
core::Expected<core::Optional<InstalledRelease>, std::string> ReadInstalledRelease(
    const std::string& path);

/// Rust `overlaybd_tools_present` — every tool must be a regular file.
bool ToolsPresent(const std::string& overlaybd_dir);

/// Rust `install_overlaybd_release_tools` — stages the extracted `bin/`, marks
/// the tools executable, swaps it into place atomically, and drops the legacy
/// `lib/` directory left by pre-static releases.
core::Expected<core::Unit, std::string> InstallReleaseTools(const std::string& extracted_root,
                                                            const std::string& overlaybd_dir);

/// Rust `stage_overlaybd_default_config` — copies the packaged
/// `etc/overlaybd/overlaybd.json` into the install directory.
core::Expected<core::Unit, std::string> StageDefaultConfig(const std::string& extracted_root,
                                                           const std::string& overlaybd_dir);

/// Rust `install_default_config`.
///
/// An existing destination's *content* is retained (an operator may have tuned
/// it), but its group and mode are always reasserted so the runtime account
/// can read it.
core::Expected<core::Unit, std::string> InstallDefaultConfig(const std::string& source,
                                                             const std::string& destination,
                                                             uint32_t runtime_gid);

/// Rust `install_system_default_config` — installs to
/// `/etc/overlaybd/overlaybd.json` from the staged copy under `deps_path`.
core::Expected<core::Unit, std::string> InstallSystemDefaultConfig(const std::string& deps_path,
                                                                   uint32_t runtime_gid);

/// Rust `extract_overlaybd_package` — only `.tar.gz` is accepted; any other
/// suffix is rejected rather than guessed at.
core::Expected<core::Unit, std::string> ExtractPackage(const std::string& package_path,
                                                       const std::string& destination);

/// Rust `ensure_release_tools` — the full install: skip when already current,
/// otherwise download, extract, install, stage the config and write the
/// marker.
core::Expected<core::Unit, std::string> EnsureReleaseTools(const DependencyConfig& config,
                                                           const std::string& overlaybd_dir,
                                                           const std::string& arch);

/// Rust `ensure_tools_converter_v1` — OCI tools v1 must keep the same block
/// layout across runtime upgrades, so when the current install has moved on,
/// a pinned copy is provisioned alongside it. Returns the directory to use.
core::Expected<std::string, std::string> EnsureToolsConverterV1(const std::string& deps_path);

}  // namespace overlaybd
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_OVERLAYBD_H_
