// SPDX-License-Identifier: MIT
// Rust: src/setup/deps.rs — the shared helpers used by the dependency
// provisioning paths (and by `setup/overlaybd`).
//
// Porting note on the two external capabilities Rust gets from crates:
//
//   * `download_file` uses `reqwest`. There is no HTTP client in this tree, so
//     the port shells out to `curl`. That is not a shortcut: the bundled
//     `config/deps_manifest.toml` already declares `curl` a *required* runtime
//     command, so relying on it adds no new dependency and keeps the build
//     free of third-party libraries.
//   * archive extraction uses `flate2` + `tar`. The port shells out to `tar`,
//     a POSIX-standard tool, for the same reason.
//
// Both are invoked through `core::process`, i.e. argv is passed straight to
// `execvp` with no shell, so a URL or path can never be interpreted as a
// command.
#ifndef AGENTENV_SETUP_DEPS_H_
#define AGENTENV_SETUP_DEPS_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/core/virtualization.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace setup {
namespace deps {

/// Rust `detect_arch` — only the two architectures AgentENV releases are
/// built for; anything else is a hard error rather than a guess.
core::Expected<std::string, std::string> DetectArch();

/// Rust `set_file_mode`.
core::Expected<core::Unit, std::string> SetFileMode(const std::string& path, uint32_t mode);

/// Rust `set_executable` — 0755.
core::Expected<core::Unit, std::string> SetExecutable(const std::string& path);

/// Rust `copy_file` — creates the destination's parent, copies, and optionally
/// marks the result executable.
core::Expected<core::Unit, std::string> CopyFile(const std::string& source,
                                                 const std::string& destination,
                                                 bool executable);

/// Rust `file_exists_nonempty` — a zero-length file counts as absent, because
/// that is what a truncated earlier download leaves behind.
bool FileExistsNonEmpty(const std::string& path);

/// Rust `download_file`.
///
/// Skips the transfer when the destination already exists and is non-empty.
/// Downloads to a sibling `.tmp` and renames, so a partial transfer never
/// appears under the final name.
core::Expected<core::Unit, std::string> DownloadFile(const std::string& url,
                                                     const std::string& destination);

/// Rust `download_executable_file`.
core::Expected<core::Unit, std::string> DownloadExecutableFile(const std::string& url,
                                                               const std::string& destination);

/// Extracts a `.tar.gz` into `destination`, which is created if needed.
/// Replaces Rust's `GzDecoder` + `tar::Archive::unpack`.
core::Expected<core::Unit, std::string> ExtractTarGz(const std::string& archive_path,
                                                     const std::string& destination);

/// Rust `copy_dir_recursive`.
core::Expected<core::Unit, std::string> CopyDirRecursive(const std::string& source,
                                                         const std::string& destination);

/// Sets the group owner without touching the user, i.e. Rust
/// `chown(path, None, Some(gid))`.
core::Expected<core::Unit, std::string> SetGroupOwner(const std::string& path, uint32_t gid);

// ---------------------------------------------------------------------------
// Dependency manifest
// ---------------------------------------------------------------------------

/// Rust struct `ManifestDownload`.
struct ManifestDownload {
    std::string version;
    std::string url;
};

/// Rust struct `ManifestVirtualizationDownloads` — a download per host
/// virtualization mode.
struct ManifestVirtualizationDownloads {
    ManifestDownload kvm;
    ManifestDownload pvm;

    /// Rust `for_mode`.
    const ManifestDownload& ForMode(core::VirtualizationMode mode) const;
};

/// Rust struct `SetupDependencyManifest` (the non-`packages` half; the package
/// lists live in `setup/packages.h`).
struct Manifest {
    ManifestVirtualizationDownloads firecracker;
    ManifestVirtualizationDownloads kernel;
    /// Rust `ManifestTools` — just a URL template.
    std::string tools_url;
    cfg::OverlaybdDependencyConfig overlaybd;
    /// Rust `#[serde(rename = "regclient")] regctl`.
    ManifestDownload regctl;

    static core::Expected<Manifest, std::string> ParseString(const std::string& toml_text);
    static core::Expected<Manifest, std::string> ParseFile(const std::string& path);
};

/// Rust `resolve_url` — substitutes every `{key}` occurrence.
std::string ResolveUrl(const std::string& templ,
                       const std::vector<std::pair<std::string, std::string> >& vars);

/// Rust `validate_explicit_file` — an operator-provided path must be a
/// non-empty, readable regular file, and executable when required.
core::Expected<core::Unit, std::string> ValidateExplicitFile(const std::string& config_key,
                                                             const std::string& path,
                                                             bool executable);

/// Rust `version_output_mentions_exact_token` — true when `version` appears as
/// a standalone token, so `v0.11.5` does not match `v0.11.50`.
bool VersionOutputMentionsExactToken(const std::string& output, const std::string& version);

/// Rust `walkdir` — files only, depth-limited to 3 like upstream.
core::Expected<std::vector<std::string>, std::string> WalkDir(const std::string& dir);

/// Rust `find_firecracker_binary` — prefers `{dir}/firecracker`, then searches,
/// excluding the sibling tools (`jailer`, `rebase-snap`) and debug artifacts.
core::Expected<std::string, std::string> FindFirecrackerBinary(const std::string& dir);

/// Rust `find_cpu_template_helper` — optional, hence an empty optional rather
/// than an error when absent.
core::Optional<std::string> FindCpuTemplateHelper(const std::string& dir);

/// Rust `extract_firecracker` — unpacks the release and installs the
/// firecracker binary plus the optional cpu-template-helper.
core::Expected<core::Unit, std::string> ExtractFirecracker(const std::string& tgz_path,
                                                           const std::string& fc_path,
                                                           const std::string& cth_path);

/// Rust `ensure_firecracker`.
core::Expected<core::Unit, std::string> EnsureFirecracker(const cfg::AppConfig& config,
                                                          const Manifest& manifest,
                                                          const std::string& arch);

/// Rust `ensure_kernel`.
core::Expected<core::Unit, std::string> EnsureKernel(const cfg::AppConfig& config,
                                                     const Manifest& manifest,
                                                     const std::string& arch);

/// Rust `ensure_regctl` — skipped when the installed binary already reports
/// the pinned version.
core::Expected<core::Unit, std::string> EnsureRegctl(const std::string& deps_path,
                                                     const Manifest& manifest);

/// Rust `ensure_tools` — validates the tools config and installs an explicitly
/// configured drive.
core::Expected<core::Unit, std::string> EnsureTools(const cfg::AppConfig& config);

/// Rust `install_explicit_tools_drive`.
core::Expected<core::Unit, std::string> InstallExplicitToolsDrive(const std::string& source,
                                                                  const std::string& destination,
                                                                  const std::string& version);

/// Rust `verify_installed_tools_drive` — a content mismatch under the same
/// version is a hard error: silently serving different bytes for one version
/// would make sandboxes non-reproducible.
core::Expected<core::Unit, std::string> VerifyInstalledToolsDrive(
    const std::string& source, const std::string& destination, const std::string& version);

/// Rust `extract_ext4_from_ghcr` — pulls an image with `regctl` into an OCI
/// layout, unpacks it with `umoci`, and lifts one file out. Deliberately
/// avoids needing a Docker daemon.
core::Expected<core::Unit, std::string> ExtractExt4FromGhcr(const std::string& regctl_path,
                                                            const std::string& image,
                                                            const std::string& filename,
                                                            const std::string& destination);

// ---------------------------------------------------------------------------
// Generated overlaybd global configs
// ---------------------------------------------------------------------------

/// Rust `detect_docker_credential_config` — `$DOCKER_CONFIG/config.json` then
/// `$HOME/.docker/config.json`. Absent means registry auth stays anonymous,
/// which is fine for public registries.
core::Optional<std::string> DetectDockerCredentialConfig();

/// Rust `overlaybd_credential_fields` — the `credentialFilePath` /
/// `credentialConfig` pair, including the specific empty-state shape upstream
/// writes (`mode: ""`, `timeout: 1`).
void OverlaybdCredentialFields(const core::Optional<std::string>& credential_path,
                               std::string* credential_file_path,
                               core::Json* credential_config);

/// Rust `overlaybd_runtime_oss_config`.
core::Expected<core::Json, std::string> OverlaybdRuntimeOssConfig(
    const cfg::OssBackendConfig& oss);

/// Rust `write_generated_overlaybd_global_config` — one config file. Written
/// 0600 because it can embed static credentials.
core::Expected<core::Unit, std::string> WriteGeneratedOverlaybdGlobalConfig(
    const std::string& path, const cfg::AppConfig& config,
    const cfg::ResolvedImageCacheConfig& image_cache,
    const core::Optional<std::string>& p2p_facade_address,
    const storage::overlaybd::DownloadConfig& download);

/// Rust `write_generated_overlaybd_global_configs` — all four configs.
///
/// The conversion and resize tools each get an isolated `cacheDir`: the C++
/// file cache treats every file under it as a flat cache entry and evicts them
/// by truncate+unlink, which would destroy the Rust runtime cache's per-entry
/// directories if they shared `remote-blocks`.
core::Expected<core::Unit, std::string> WriteGeneratedOverlaybdGlobalConfigs(
    const cfg::AppConfig& config, const core::Optional<std::string>& p2p_facade_address);

}  // namespace deps
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_DEPS_H_
