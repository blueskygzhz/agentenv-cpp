// SPDX-License-Identifier: MIT
// Rust: src/setup/overlaybd.rs
#include "agentenv/setup/overlaybd.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <sstream>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/setup/deps.h"

namespace agentenv {
namespace setup {
namespace overlaybd {

const char* const kConverterV1Version = "v1.0.18-aenv.1";
const char* const kConverterV1PackageUrl =
    "https://github.com/kvcache-ai/overlaybd/releases/download/static-{version}/"
    "overlaybd-tools-{version}-linux-{arch}.tar.gz";

namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

/// Replaces every occurrence of `needle` in `text`.
std::string ReplaceAll(const std::string& text, const std::string& needle,
                       const std::string& replacement) {
    if (needle.empty()) return text;
    std::string out;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t found = text.find(needle, cursor);
        if (found == std::string::npos) {
            out += text.substr(cursor);
            return out;
        }
        out += text.substr(cursor, found - cursor);
        out += replacement;
        cursor = found + needle.size();
    }
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Rust `url.rsplit('/').next()` — the last path segment.
std::string AssetNameFromUrl(const std::string& url) {
    const std::size_t slash = url.rfind('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
}

/// Creates a uniquely named sibling directory, standing in for
/// `tempfile::Builder::tempdir_in`.
core::Expected<std::string, std::string> TempDirIn(const std::string& parent,
                                                   const std::string& prefix) {
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(parent);
    if (!made.ok()) return core::make_unexpected(made.error());

    std::string pattern = fs::Join(parent, prefix + "XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(&buffer[0]) == NULL) {
        std::ostringstream oss;
        oss << "create temp dir in '" << parent << "': " << std::strerror(errno);
        return core::make_unexpected(oss.str());
    }
    return std::string(&buffer[0]);
}

/// Rust `remove_legacy_overlaybd_lib` — pre-static releases shipped a `lib/`
/// directory; leaving it behind would let the dynamic loader pick up stale
/// libraries next to the new static tools.
core::Expected<Unit, std::string> RemoveLegacyLib(const std::string& overlaybd_dir) {
    const std::string legacy_lib = fs::Join(overlaybd_dir, "lib");

    const core::Expected<fs::FileStat, std::string> stat = fs::SymlinkStat(legacy_lib);
    if (!stat.ok()) {
        // Absent is the normal case for a fresh install.
        if (!fs::Exists(legacy_lib)) return Unit();
        return core::make_unexpected(std::string("inspect legacy overlaybd lib ") + legacy_lib +
                                     ": " + stat.error());
    }

    core::Expected<Unit, std::string> removed = Unit();
    if (stat.value().is_dir && !stat.value().is_symlink) {
        removed = fs::RemoveDirAll(legacy_lib);
    } else {
        removed = fs::RemoveFile(legacy_lib);
    }
    if (!removed.ok()) {
        return core::make_unexpected(std::string("remove legacy overlaybd lib ") + legacy_lib +
                                     ": " + removed.error());
    }
    return Unit();
}

/// Rust `replace_overlaybd_release_bin` + `swap_failure`.
///
/// The swap is rename-based so a reader never sees a half-installed `bin/`.
/// If installing the staged directory fails, the previous one is renamed back;
/// if *that* also fails, the backup is deliberately left on disk and named in
/// the error, because an operator can still recover from it by hand.
core::Expected<Unit, std::string> ReplaceReleaseBin(const std::string& overlaybd_dir,
                                                    const std::string& staged_bin) {
    const core::Expected<std::string, std::string> backup =
        TempDirIn(overlaybd_dir, "release-backup-");
    if (!backup.ok()) {
        return core::make_unexpected(std::string("create overlaybd release backup dir: ") +
                                     backup.error());
    }

    const std::string target_bin = fs::Join(overlaybd_dir, "bin");
    const std::string backup_bin = fs::Join(backup.value(), "bin");
    const bool had_bin = fs::Exists(target_bin);

    if (had_bin && ::rename(target_bin.c_str(), backup_bin.c_str()) != 0) {
        const std::string error = std::strerror(errno);
        fs::RemoveDirAll(backup.value());
        return core::make_unexpected(std::string("backup installed overlaybd bin dir: ") + error);
    }

    if (::rename(staged_bin.c_str(), target_bin.c_str()) == 0) {
        fs::RemoveDirAll(backup.value());
        return Unit();
    }

    const std::string install_error = std::strerror(errno);
    if (!had_bin) {
        fs::RemoveDirAll(backup.value());
        return core::make_unexpected(std::string("install staged overlaybd bin dir: ") +
                                     install_error);
    }

    if (::rename(backup_bin.c_str(), target_bin.c_str()) == 0) {
        fs::RemoveDirAll(backup.value());
        return core::make_unexpected(std::string("install staged overlaybd bin dir: ") +
                                     install_error);
    }

    // Rollback failed too. Keep the backup and say where it is.
    std::ostringstream oss;
    oss << "install staged overlaybd bin dir: " << install_error
        << ": overlaybd release rollback was incomplete: " << std::strerror(errno)
        << "; previous release backup preserved at '" << backup.value() << "'";
    return core::make_unexpected(oss.str());
}

}  // namespace

const std::vector<std::string>& ToolNames() {
    static const std::vector<std::string>* const kNames = []() {
        std::vector<std::string>* out = new std::vector<std::string>();
        out->push_back("overlaybd-create");
        out->push_back("overlaybd-apply");
        out->push_back("overlaybd-commit");
        out->push_back("overlaybd-resize");
        return out;
    }();
    return *kNames;
}

std::string InstalledRelease::ToJson() const {
    core::JsonObject root;
    root["tag_name"] = core::Json(tag_name);
    root["asset_name"] = core::Json(asset_name);
    // Serde writes `null` for a `None` here (no `skip_serializing_if`), and
    // the upstream test fixture contains `"digest":null`.
    root["digest"] = digest.has_value() ? core::Json(*digest) : core::Json();
    return core::Json(root).ToString();
}

core::Expected<InstalledRelease, std::string> InstalledRelease::ParseJson(
    const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) {
        return core::make_unexpected(std::string("parse overlaybd release metadata: ") +
                                     parsed.error().chain());
    }
    if (parsed.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(
            std::string("overlaybd release metadata must be a JSON object"));
    }

    const core::JsonObject& members = parsed.value().as_object();
    InstalledRelease release;

    const core::JsonObject::const_iterator tag = members.find("tag_name");
    if (tag == members.end() || tag->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(
            std::string("overlaybd release metadata is missing a string 'tag_name'"));
    }
    release.tag_name = tag->second.as_string();

    const core::JsonObject::const_iterator asset = members.find("asset_name");
    if (asset == members.end() || asset->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(
            std::string("overlaybd release metadata is missing a string 'asset_name'"));
    }
    release.asset_name = asset->second.as_string();

    const core::JsonObject::const_iterator digest_field = members.find("digest");
    if (digest_field != members.end() &&
        digest_field->second.kind() == core::Json::Kind::String) {
        release.digest = digest_field->second.as_string();
    }
    return release;
}

bool InstalledRelease::operator==(const InstalledRelease& o) const {
    if (tag_name != o.tag_name || asset_name != o.asset_name) return false;
    if (digest.has_value() != o.digest.has_value()) return false;
    return !digest.has_value() || *digest == *o.digest;
}

core::Expected<ConfiguredRelease, std::string> ConfiguredReleaseFor(
    const DependencyConfig& config, const std::string& arch) {
    const std::string tag_name = Trim(config.version);
    if (tag_name.empty()) {
        return core::make_unexpected(std::string("overlaybd.version not set in config"));
    }

    // `package_url` wins over the legacy `url`.
    std::string template_url;
    if (config.package_url.has_value()) {
        template_url = *config.package_url;
    } else if (config.url.has_value()) {
        template_url = *config.url;
    } else {
        return core::make_unexpected(std::string("overlaybd.package_url not set in config"));
    }
    template_url = Trim(template_url);
    if (template_url.empty()) {
        return core::make_unexpected(std::string("overlaybd.package_url not set in config"));
    }

    if (arch != "x86_64" && arch != "aarch64") {
        return core::make_unexpected(
            std::string("unsupported overlaybd release architecture: ") + arch);
    }

    ConfiguredRelease release;
    release.tag_name = tag_name;
    release.package_url = ReplaceAll(ReplaceAll(template_url, "{version}", tag_name),
                                     "{arch}", arch);
    return release;
}

InstalledRelease DesiredInstalledRelease(const ConfiguredRelease& release,
                                         const std::string& asset_name) {
    InstalledRelease desired;
    desired.tag_name = release.tag_name;
    desired.asset_name = asset_name;
    return desired;  // digest stays absent, as upstream
}

core::Expected<Optional<InstalledRelease>, std::string> ReadInstalledRelease(
    const std::string& path) {
    if (!fs::Exists(path)) return Optional<InstalledRelease>();

    const core::Expected<std::string, std::string> bytes = fs::ReadToString(path);
    if (!bytes.ok()) {
        return core::make_unexpected(std::string("read overlaybd release metadata '") + path +
                                     "': " + bytes.error());
    }
    const core::Expected<InstalledRelease, std::string> release =
        InstalledRelease::ParseJson(bytes.value());
    if (!release.ok()) {
        return core::make_unexpected(std::string("parse overlaybd release metadata '") + path +
                                     "': " + release.error());
    }
    return Optional<InstalledRelease>(release.value());
}

bool ToolsPresent(const std::string& overlaybd_dir) {
    const std::vector<std::string>& tools = ToolNames();
    const std::string bin = fs::Join(overlaybd_dir, "bin");
    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (!fs::IsFile(fs::Join(bin, tools[i]))) return false;
    }
    return true;
}

core::Expected<Unit, std::string> ExtractPackage(const std::string& package_path,
                                                 const std::string& destination) {
    const Optional<std::string> name = fs::FileName(package_path);
    const std::string package_name = name.has_value() ? *name : std::string();

    if (EndsWith(package_name, ".tar.gz")) {
        return deps::ExtractTarGz(package_path, destination);
    }
    std::ostringstream oss;
    oss << "unsupported overlaybd package format for " << package_path << " (expected .tar.gz)";
    return core::make_unexpected(oss.str());
}

core::Expected<Unit, std::string> InstallReleaseTools(const std::string& extracted_root,
                                                      const std::string& overlaybd_dir) {
    const std::string source_bin_dir = fs::Join(extracted_root, "bin");
    if (!fs::IsDir(source_bin_dir)) {
        std::ostringstream oss;
        oss << "overlaybd release payload missing bin dir at " << source_bin_dir;
        return core::make_unexpected(oss.str());
    }

    const core::Expected<Unit, std::string> made = fs::CreateDirAll(overlaybd_dir);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create overlaybd dir '") + overlaybd_dir +
                                     "': " + made.error());
    }

    // Staged inside the install directory, so the final swap is a rename
    // within one filesystem and therefore atomic.
    const core::Expected<std::string, std::string> staging =
        TempDirIn(overlaybd_dir, "release-staging-");
    if (!staging.ok()) {
        return core::make_unexpected(std::string("create overlaybd release staging dir: ") +
                                     staging.error());
    }

    const std::string staged_bin = fs::Join(staging.value(), "bin");
    const core::Expected<Unit, std::string> copied =
        deps::CopyDirRecursive(source_bin_dir, staged_bin);
    if (!copied.ok()) {
        fs::RemoveDirAll(staging.value());
        return copied;
    }

    const std::vector<std::string>& tools = ToolNames();
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const std::string staged = fs::Join(staged_bin, tools[i]);
        if (!fs::IsFile(staged)) {
            fs::RemoveDirAll(staging.value());
            std::ostringstream oss;
            oss << "overlaybd release payload missing required tool '"
                << fs::Join(source_bin_dir, tools[i]) << "'";
            return core::make_unexpected(oss.str());
        }
        const core::Expected<Unit, std::string> executable = deps::SetExecutable(staged);
        if (!executable.ok()) {
            fs::RemoveDirAll(staging.value());
            return executable;
        }
    }

    const core::Expected<Unit, std::string> replaced =
        ReplaceReleaseBin(overlaybd_dir, staged_bin);
    fs::RemoveDirAll(staging.value());
    if (!replaced.ok()) return replaced;

    // Downgraded to a warning upstream: the new static tools work regardless,
    // so a stuck legacy directory must not fail the install.
    const core::Expected<Unit, std::string> legacy = RemoveLegacyLib(overlaybd_dir);
    if (!legacy.ok()) {
        AGENTENV_WARN("failed to remove legacy overlaybd libraries; continuing with installed "
                      "static tools path="
                      << fs::Join(overlaybd_dir, "lib") << " error=" << legacy.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> StageDefaultConfig(const std::string& extracted_root,
                                                     const std::string& overlaybd_dir) {
    const std::string packaged =
        fs::Join(extracted_root, "etc/overlaybd/overlaybd.json");
    if (!fs::IsFile(packaged)) {
        std::ostringstream oss;
        oss << "overlaybd release payload missing default config at " << packaged;
        return core::make_unexpected(oss.str());
    }

    const std::string staged = fs::Join(overlaybd_dir, "etc/overlaybd/overlaybd.json");
    const Optional<std::string> parent = fs::Parent(staged);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create overlaybd config dir '") + *parent +
                                         "': " + made.error());
        }
    }

    const core::Expected<Unit, std::string> copied = fs::Copy(packaged, staged);
    if (!copied.ok()) {
        std::ostringstream oss;
        oss << "stage overlaybd default config '" << packaged << "' -> '" << staged
            << "': " << copied.error();
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

core::Expected<Unit, std::string> InstallDefaultConfig(const std::string& source,
                                                       const std::string& destination,
                                                       uint32_t runtime_gid) {
    if (fs::Exists(destination) && !fs::IsFile(destination)) {
        std::ostringstream oss;
        oss << "overlaybd system config is not a regular file: " << destination;
        return core::make_unexpected(oss.str());
    }

    const Optional<std::string> parent = fs::Parent(destination);
    if (!parent.has_value()) {
        return core::make_unexpected(
            std::string("overlaybd system config path has no parent directory"));
    }

    const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create ") + *parent + ": " + made.error());
    }
    const core::Expected<Unit, std::string> parent_group =
        deps::SetGroupOwner(*parent, runtime_gid);
    if (!parent_group.ok()) return parent_group;
    const core::Expected<Unit, std::string> parent_mode = deps::SetFileMode(*parent, 0750);
    if (!parent_mode.ok()) return parent_mode;

    if (fs::IsFile(destination)) {
        // An operator may have tuned this file; its content is kept and only
        // the ownership/mode below are reasserted.
        AGENTENV_INFO("retaining existing overlaybd system config content path=" << destination);
    } else if (!fs::IsFile(source)) {
        std::ostringstream oss;
        oss << "staged overlaybd default config is missing: " << source;
        return core::make_unexpected(oss.str());
    } else {
        const core::Expected<Unit, std::string> copied = fs::Copy(source, destination);
        if (!copied.ok()) {
            std::ostringstream oss;
            oss << "install overlaybd default config " << source << " -> " << destination << ": "
                << copied.error();
            return core::make_unexpected(oss.str());
        }
    }

    const core::Expected<Unit, std::string> group =
        deps::SetGroupOwner(destination, runtime_gid);
    if (!group.ok()) return group;
    return deps::SetFileMode(destination, 0640);
}

core::Expected<Unit, std::string> InstallSystemDefaultConfig(const std::string& deps_path,
                                                             uint32_t runtime_gid) {
    const std::string source =
        fs::Join(deps_path, "overlaybd/etc/overlaybd/overlaybd.json");
    return InstallDefaultConfig(source, "/etc/overlaybd/overlaybd.json", runtime_gid);
}

core::Expected<Unit, std::string> EnsureReleaseTools(const DependencyConfig& config,
                                                     const std::string& overlaybd_dir,
                                                     const std::string& arch) {
    // Different tools releases can request the same converter concurrently.
    // Serialize the complete installation, including its cache check and
    // cleanup — Rust uses a `tokio::sync::Mutex` for exactly this.
    static std::mutex install_mutex;
    std::lock_guard<std::mutex> guard(install_mutex);

    const core::Expected<ConfiguredRelease, std::string> configured =
        ConfiguredReleaseFor(config, arch);
    if (!configured.ok()) return core::make_unexpected(configured.error());

    const std::string asset_name = AssetNameFromUrl(configured.value().package_url);
    if (asset_name.empty()) {
        return core::make_unexpected(std::string("overlaybd package URL missing asset name"));
    }
    const InstalledRelease desired = DesiredInstalledRelease(configured.value(), asset_name);

    const std::string metadata_path = fs::Join(overlaybd_dir, "tools-release.json");
    const core::Expected<Optional<InstalledRelease>, std::string> installed =
        ReadInstalledRelease(metadata_path);
    if (!installed.ok()) return core::make_unexpected(installed.error());

    const std::string staged_default_config =
        fs::Join(overlaybd_dir, "etc/overlaybd/overlaybd.json");

    // All three conditions must hold: the marker matches, every tool is
    // present, and the staged config exists. Any one missing means the
    // previous install was incomplete.
    if (installed.value().has_value() && *installed.value() == desired &&
        ToolsPresent(overlaybd_dir) && fs::IsFile(staged_default_config)) {
        AGENTENV_DEBUG("overlaybd CLI tools already installed tag="
                       << configured.value().tag_name << " asset=" << asset_name);
        return Unit();
    }

    const std::string downloads_dir = fs::Join(overlaybd_dir, "downloads");
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(downloads_dir);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create overlaybd downloads dir '") +
                                     downloads_dir + "': " + made.error());
    }

    const std::string package_path = fs::Join(downloads_dir, asset_name);
    const core::Expected<Unit, std::string> downloaded =
        deps::DownloadFile(configured.value().package_url, package_path);
    if (!downloaded.ok()) return downloaded;

    const core::Expected<std::string, std::string> extract_dir =
        fs::CreateTempDir("agentenv-overlaybd-release-");
    if (!extract_dir.ok()) {
        return core::make_unexpected(std::string("create temp dir for overlaybd release: ") +
                                     extract_dir.error());
    }

    core::Expected<Unit, std::string> result = ExtractPackage(package_path, extract_dir.value());
    if (result.ok()) result = InstallReleaseTools(extract_dir.value(), overlaybd_dir);
    if (result.ok()) result = StageDefaultConfig(extract_dir.value(), overlaybd_dir);
    fs::RemoveDirAll(extract_dir.value());
    if (!result.ok()) return result;

    const core::Expected<Unit, std::string> marker =
        fs::Write(metadata_path, desired.ToJson());
    if (!marker.ok()) {
        return core::make_unexpected(std::string("write overlaybd release metadata '") +
                                     metadata_path + "': " + marker.error());
    }

    // The archive is only needed during extraction; removing it (and the
    // now-empty downloads dir) keeps `--setup-only` container layers small.
    fs::RemoveFile(package_path);
    ::rmdir(downloads_dir.c_str());
    return Unit();
}

core::Expected<std::string, std::string> EnsureToolsConverterV1(const std::string& deps_path) {
    const core::Expected<std::string, std::string> arch = deps::DetectArch();
    if (!arch.ok()) return core::make_unexpected(arch.error());

    DependencyConfig release;
    release.version = kConverterV1Version;
    release.package_url = std::string(kConverterV1PackageUrl);

    const std::string current = fs::Join(deps_path, "overlaybd");
    const core::Expected<ConfiguredRelease, std::string> configured =
        ConfiguredReleaseFor(release, arch.value());
    if (!configured.ok()) return core::make_unexpected(configured.error());

    std::ostringstream asset;
    asset << "overlaybd-tools-" << release.version << "-linux-" << arch.value() << ".tar.gz";
    const InstalledRelease expected =
        DesiredInstalledRelease(configured.value(), asset.str());

    // When the runtime install still *is* v1, reuse it rather than duplicating
    // the tools; converter v1 must keep the same block layout, so a runtime
    // that has moved on cannot be shared.
    const core::Expected<Optional<InstalledRelease>, std::string> installed =
        ReadInstalledRelease(fs::Join(current, "tools-release.json"));
    if (!installed.ok()) return core::make_unexpected(installed.error());
    if (installed.value().has_value() && *installed.value() == expected &&
        ToolsPresent(current)) {
        return current;
    }

    const std::string pinned = fs::Join(deps_path, "tools-converter-v1");
    const core::Expected<Unit, std::string> ensured =
        EnsureReleaseTools(release, pinned, arch.value());
    if (!ensured.ok()) return core::make_unexpected(ensured.error());
    return pinned;
}

}  // namespace overlaybd
}  // namespace setup
}  // namespace agentenv
