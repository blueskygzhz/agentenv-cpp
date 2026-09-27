// SPDX-License-Identifier: MIT
// Rust: src/setup/deps.rs — shared helpers.
#include "agentenv/setup/deps.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include <unistd.h>

#include "agentenv/core/credentials.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/core/toml.h"

namespace agentenv {
namespace setup {
namespace deps {
namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;
namespace process = core::process;

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

}  // namespace

core::Expected<std::string, std::string> DetectArch() {
#if defined(__x86_64__)
    return std::string("x86_64");
#elif defined(__aarch64__)
    return std::string("aarch64");
#else
    // Rust reports `std::env::consts::ARCH`; there is no portable equivalent,
    // so the message says what it can.
    return core::make_unexpected(
        std::string("unsupported architecture: this binary was not built for x86_64 or aarch64"));
#endif
}

core::Expected<Unit, std::string> SetFileMode(const std::string& path, uint32_t mode) {
    const core::Expected<Unit, std::string> applied = fs::SetPermissions(path, mode);
    if (!applied.ok()) {
        return core::make_unexpected(std::string("set permissions on ") + path + ": " +
                                     applied.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> SetExecutable(const std::string& path) {
    return SetFileMode(path, 0755);
}

core::Expected<Unit, std::string> CopyFile(const std::string& source,
                                           const std::string& destination, bool executable) {
    const Optional<std::string> parent = fs::Parent(destination);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create target dir '") + *parent + "': " +
                                         made.error());
        }
    }

    const core::Expected<Unit, std::string> copied = fs::Copy(source, destination);
    if (!copied.ok()) {
        return core::make_unexpected(std::string("copy '") + source + "' to '" + destination +
                                     "': " + copied.error());
    }
    if (executable) return SetExecutable(destination);
    return Unit();
}

bool FileExistsNonEmpty(const std::string& path) {
    if (!fs::IsFile(path)) return false;
    const core::Expected<uint64_t, std::string> size = fs::FileSize(path);
    return size.ok() && size.value() > 0;
}

core::Expected<Unit, std::string> DownloadFile(const std::string& url,
                                               const std::string& destination) {
    if (FileExistsNonEmpty(destination)) {
        AGENTENV_DEBUG("already present, skipping download path=" << destination);
        return Unit();
    }

    const Optional<std::string> parent = fs::Parent(destination);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) return core::make_unexpected(made.error());
    }

    AGENTENV_INFO("downloading url=" << url << " dest=" << destination);

    // Rust writes to `dest.with_extension("tmp")` and renames, so a partial
    // transfer never appears under the final name. Same here.
    const std::string temporary = destination + ".tmp";
    fs::RemoveFile(temporary);

    std::vector<std::string> argv;
    argv.push_back("curl");
    argv.push_back("--fail");           // non-2xx becomes a non-zero exit
    argv.push_back("--location");       // follow redirects, as reqwest does
    argv.push_back("--silent");
    argv.push_back("--show-error");
    argv.push_back("--output");
    argv.push_back(temporary);
    argv.push_back(url);

    const core::Expected<process::Output, std::string> output = process::Run(argv);
    if (!output.ok()) {
        fs::RemoveFile(temporary);
        return core::make_unexpected(std::string("GET ") + url + ": " + output.error());
    }
    if (!output.value().success()) {
        fs::RemoveFile(temporary);
        std::ostringstream oss;
        // `--fail` turns an HTTP error into exit 22, which is the closest
        // analogue of Rust's `HTTP {status} for {url}`.
        oss << "GET " << url << " failed (" << output.value().StatusString() << ")";
        const std::string stderr_text = output.value().stderr_text;
        if (!stderr_text.empty()) oss << ": " << stderr_text;
        return core::make_unexpected(oss.str());
    }

    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        const int saved = errno;
        fs::RemoveFile(temporary);
        return core::make_unexpected(std::string("moving downloaded file to destination: ") +
                                     std::strerror(saved));
    }
    return Unit();
}

core::Expected<Unit, std::string> DownloadExecutableFile(const std::string& url,
                                                         const std::string& destination) {
    const core::Expected<Unit, std::string> downloaded = DownloadFile(url, destination);
    if (!downloaded.ok()) return downloaded;
    return SetExecutable(destination);
}

core::Expected<Unit, std::string> ExtractTarGz(const std::string& archive_path,
                                               const std::string& destination) {
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(destination);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create extraction dir '") + destination +
                                     "': " + made.error());
    }

    std::vector<std::string> argv;
    argv.push_back("tar");
    argv.push_back("-xzf");
    argv.push_back(archive_path);
    argv.push_back("-C");
    argv.push_back(destination);

    const core::Expected<process::Output, std::string> output = process::Run(argv);
    if (!output.ok()) {
        return core::make_unexpected(std::string("extract overlaybd package ") + archive_path +
                                     ": " + output.error());
    }
    if (!output.value().success()) {
        std::ostringstream oss;
        oss << "extract overlaybd package " << archive_path << " ("
            << output.value().StatusString() << ")";
        if (!output.value().stderr_text.empty()) oss << ": " << output.value().stderr_text;
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

core::Expected<Unit, std::string> CopyDirRecursive(const std::string& source,
                                                   const std::string& destination) {
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(destination);
    if (!made.ok()) {
        return core::make_unexpected(std::string("create overlaybd lib dir '") + destination +
                                     "': " + made.error());
    }

    const core::Expected<std::vector<std::string>, std::string> entries = fs::ReadDir(source);
    if (!entries.ok()) {
        return core::make_unexpected(std::string("read overlaybd lib dir '") + source + "': " +
                                     entries.error());
    }

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const std::string& name = entries.value()[i];
        const std::string entry_path = fs::Join(source, name);
        const std::string destination_path = fs::Join(destination, name);

        if (fs::IsDir(entry_path)) {
            const core::Expected<Unit, std::string> nested =
                CopyDirRecursive(entry_path, destination_path);
            if (!nested.ok()) return nested;
        } else {
            const core::Expected<Unit, std::string> copied =
                CopyFile(entry_path, destination_path, false);
            if (!copied.ok()) return copied;
        }
    }
    return Unit();
}

core::Expected<Unit, std::string> SetGroupOwner(const std::string& path, uint32_t gid) {
    // -1 for the uid leaves the owner untouched, matching `chown(path, None,
    // Some(gid))`.
    if (::chown(path.c_str(), static_cast<uid_t>(-1), static_cast<gid_t>(gid)) != 0) {
        std::ostringstream oss;
        oss << "set runtime group ownership on " << path << ": " << std::strerror(errno);
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

const ManifestDownload& ManifestVirtualizationDownloads::ForMode(
    core::VirtualizationMode mode) const {
    return mode == core::VirtualizationMode::Pvm ? pvm : kvm;
}

namespace {

/// Reads a `version` + `url` pair out of a manifest table.
core::Expected<ManifestDownload, std::string> ReadDownload(const core::TomlTable& table,
                                                           const std::string& prefix) {
    ManifestDownload download;

    const core::TomlValue* version = table.Find(prefix + ".version");
    if (version == NULL) {
        return core::make_unexpected(std::string("dependency manifest is missing '") + prefix +
                                     ".version'");
    }
    const core::Expected<std::string, std::string> version_text = version->AsString();
    if (!version_text.ok()) return core::make_unexpected(version_text.error());
    download.version = version_text.value();

    const core::TomlValue* url = table.Find(prefix + ".url");
    if (url == NULL) {
        return core::make_unexpected(std::string("dependency manifest is missing '") + prefix +
                                     ".url'");
    }
    const core::Expected<std::string, std::string> url_text = url->AsString();
    if (!url_text.ok()) return core::make_unexpected(url_text.error());
    download.url = url_text.value();

    return download;
}

core::Optional<std::string> OptionalString(const core::TomlTable& table,
                                           const std::string& key) {
    const core::TomlValue* value = table.Find(key);
    if (value == NULL) return core::Optional<std::string>();
    const core::Expected<std::string, std::string> text = value->AsString();
    if (!text.ok()) return core::Optional<std::string>();
    return core::Optional<std::string>(text.value());
}

}  // namespace

core::Expected<Manifest, std::string> Manifest::ParseString(const std::string& toml_text) {
    const core::Expected<core::TomlTable, std::string> parsed =
        core::TomlTable::ParseString(toml_text);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    const core::TomlTable& table = parsed.value();

    Manifest manifest;

    struct ModeTarget {
        const char* prefix;
        ManifestDownload* target;
    };
    const ModeTarget targets[] = {
        {"firecracker.kvm", &manifest.firecracker.kvm},
        {"firecracker.pvm", &manifest.firecracker.pvm},
        {"kernel.kvm", &manifest.kernel.kvm},
        {"kernel.pvm", &manifest.kernel.pvm},
        {"regclient", &manifest.regctl},
    };
    for (std::size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        const core::Expected<ManifestDownload, std::string> download =
            ReadDownload(table, targets[i].prefix);
        if (!download.ok()) return core::make_unexpected(download.error());
        *targets[i].target = download.value();
    }

    const core::Optional<std::string> tools_url = OptionalString(table, "tools.url");
    if (!tools_url.has_value()) {
        return core::make_unexpected(std::string("dependency manifest is missing 'tools.url'"));
    }
    manifest.tools_url = *tools_url;

    const core::Optional<std::string> overlaybd_version =
        OptionalString(table, "overlaybd.version");
    if (!overlaybd_version.has_value()) {
        return core::make_unexpected(
            std::string("dependency manifest is missing 'overlaybd.version'"));
    }
    manifest.overlaybd.version = *overlaybd_version;
    manifest.overlaybd.url = OptionalString(table, "overlaybd.url");
    manifest.overlaybd.package_url = OptionalString(table, "overlaybd.package_url");

    return manifest;
}

core::Expected<Manifest, std::string> Manifest::ParseFile(const std::string& path) {
    const core::Expected<std::string, std::string> text = fs::ReadToString(path);
    if (!text.ok()) {
        return core::make_unexpected(std::string("read dependency manifest ") + path + ": " +
                                     text.error());
    }
    return ParseString(text.value());
}

std::string ResolveUrl(const std::string& templ,
                       const std::vector<std::pair<std::string, std::string> >& vars) {
    std::string result = templ;
    for (std::size_t i = 0; i < vars.size(); ++i) {
        const std::string needle = "{" + vars[i].first + "}";
        // Replace every occurrence, not just the first: the firecracker URL
        // template uses `{version}` twice.
        std::string out;
        std::size_t cursor = 0;
        while (true) {
            const std::size_t found = result.find(needle, cursor);
            if (found == std::string::npos) {
                out += result.substr(cursor);
                break;
            }
            out += result.substr(cursor, found - cursor);
            out += vars[i].second;
            cursor = found + needle.size();
        }
        result = out;
    }
    return result;
}

core::Expected<Unit, std::string> ValidateExplicitFile(const std::string& config_key,
                                                       const std::string& path,
                                                       bool executable) {
    const core::Expected<fs::FileStat, std::string> stat = fs::Stat(path);
    if (!stat.ok()) {
        return core::make_unexpected(std::string("validate ") + config_key + " " + path + ": " +
                                     stat.error());
    }
    if (!stat.value().is_regular) {
        return core::make_unexpected(config_key + " is not a regular file: " + path);
    }
    if (stat.value().size == 0) {
        return core::make_unexpected(config_key + " is an empty file: " + path);
    }

    // Readability is checked by actually opening it: the mode bits alone do
    // not account for ACLs or a non-traversable parent.
    const core::Expected<fs::FileDescriptor, std::string> opened = fs::OpenReadFollow(path);
    if (!opened.ok()) {
        return core::make_unexpected(config_key + " is not readable: " + path);
    }

    if (executable && (stat.value().mode & 0111) == 0) {
        return core::make_unexpected(config_key + " is not executable: " + path);
    }

    AGENTENV_INFO("using local dependency config_key=" << config_key << " path=" << path);
    return Unit();
}

bool VersionOutputMentionsExactToken(const std::string& output, const std::string& version) {
    if (version.empty()) return false;

    // Tokenised on whitespace plus the punctuation `regctl version` uses, so
    // `v0.11.5` matches in `VCSTag: v0.11.5` but not inside `v0.11.50`.
    std::size_t cursor = 0;
    while (cursor <= output.size()) {
        std::size_t end = cursor;
        while (end < output.size()) {
            const char c = output[end];
            const bool separator = std::isspace(static_cast<unsigned char>(c)) || c == ':' ||
                                   c == '=' || c == ',' || c == '"' || c == '\'';
            if (separator) break;
            ++end;
        }
        if (end > cursor && output.compare(cursor, end - cursor, version) == 0) return true;
        if (end >= output.size()) break;
        cursor = end + 1;
    }
    return false;
}

namespace {

/// Rust `walkdir_inner` — depth-limited so a deep or symlinked release
/// payload cannot make the search unbounded.
core::Expected<Unit, std::string> WalkDirInner(const std::string& dir, int depth,
                                               std::vector<std::string>* results) {
    if (depth == 0) return Unit();

    const core::Expected<std::vector<std::string>, std::string> entries = fs::ReadDir(dir);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const std::string path = fs::Join(dir, entries.value()[i]);
        if (fs::IsFile(path)) {
            results->push_back(path);
        } else if (fs::IsDir(path)) {
            const core::Expected<Unit, std::string> nested =
                WalkDirInner(path, depth - 1, results);
            if (!nested.ok()) return nested;
        }
    }
    return Unit();
}

}  // namespace

core::Expected<std::vector<std::string>, std::string> WalkDir(const std::string& dir) {
    std::vector<std::string> results;
    const core::Expected<Unit, std::string> walked = WalkDirInner(dir, 3, &results);
    if (!walked.ok()) return core::make_unexpected(walked.error());
    return results;
}

core::Optional<std::string> FindCpuTemplateHelper(const std::string& dir) {
    const core::Expected<std::vector<std::string>, std::string> entries = WalkDir(dir);
    if (!entries.ok()) return core::Optional<std::string>();

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const core::Optional<std::string> name = fs::FileName(entries.value()[i]);
        if (!name.has_value()) continue;
        if (name->find("cpu-template-helper") == std::string::npos) continue;
        // `.debug` companions are not runnable binaries.
        if (name->size() >= 6 && name->compare(name->size() - 6, 6, ".debug") == 0) continue;
        return core::Optional<std::string>(entries.value()[i]);
    }
    return core::Optional<std::string>();
}

core::Expected<std::string, std::string> FindFirecrackerBinary(const std::string& dir) {
    const std::string direct = fs::Join(dir, "firecracker");
    if (fs::IsFile(direct)) return direct;

    const core::Expected<std::vector<std::string>, std::string> entries = WalkDir(dir);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        const core::Optional<std::string> name_opt = fs::FileName(entries.value()[i]);
        if (!name_opt.has_value()) continue;
        const std::string& name = *name_opt;

        if (name.find("firecracker") == std::string::npos) continue;
        // The release tarball also ships these next to the binary; picking one
        // of them would install the wrong program under the right name.
        if (name.find("jailer") != std::string::npos) continue;
        if (name.find("rebase-snap") != std::string::npos) continue;
        if (name.size() >= 6 && name.compare(name.size() - 6, 6, ".debug") == 0) continue;
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".yaml") == 0) continue;

        return entries.value()[i];
    }
    return core::make_unexpected(std::string("firecracker binary not found in ") + dir);
}

core::Expected<Unit, std::string> ExtractFirecracker(const std::string& tgz_path,
                                                     const std::string& fc_path,
                                                     const std::string& cth_path) {
    std::ostringstream name;
    name << "agentenv-fc-" << static_cast<long>(::getpid());
    const std::string tmp_dir = fs::Join("/tmp", name.str());

    const core::Expected<Unit, std::string> made = fs::CreateDirAll(tmp_dir);
    if (!made.ok()) return made;

    core::Expected<Unit, std::string> result = ExtractTarGz(tgz_path, tmp_dir);
    if (result.ok()) {
        const core::Expected<std::string, std::string> candidate =
            FindFirecrackerBinary(tmp_dir);
        if (!candidate.ok()) {
            result = core::make_unexpected(candidate.error());
        } else {
            result = CopyFile(candidate.value(), fc_path, true);
        }
    }

    if (result.ok()) {
        // cpu-template-helper is optional; a missing or unusable one must not
        // fail the firecracker install.
        const core::Optional<std::string> helper = FindCpuTemplateHelper(tmp_dir);
        if (helper.has_value() && CopyFile(*helper, cth_path, true).ok()) {
            AGENTENV_INFO("cpu-template-helper extracted path=" << cth_path);
        }
    }

    fs::RemoveDirAll(tmp_dir);
    return result;
}

core::Expected<Unit, std::string> EnsureFirecracker(const cfg::AppConfig& config,
                                                    const Manifest& manifest,
                                                    const std::string& arch) {
    // An explicitly configured binary is validated, never replaced.
    if (config.firecracker.binary_path.has_value()) {
        return ValidateExplicitFile("firecracker.binary_path",
                                    *config.firecracker.binary_path, true);
    }

    const std::string fc_path = config.ResolvedFirecrackerBinaryPath();
    if (FileExistsNonEmpty(fc_path)) {
        AGENTENV_DEBUG("firecracker binary already present path=" << fc_path);
        return Unit();
    }

    const ManifestDownload& mode_manifest =
        manifest.firecracker.ForMode(config.virtualization_mode);
    const std::string version = config.firecracker.version.has_value()
                                    ? *config.firecracker.version
                                    : mode_manifest.version;
    const std::string url_template =
        config.firecracker.url.has_value() ? *config.firecracker.url : mode_manifest.url;

    const core::Optional<std::string> fc_dir = fs::Parent(fc_path);
    if (!fc_dir.has_value()) {
        return core::make_unexpected(
            std::string("resolved firecracker binary path has no parent"));
    }

    std::ostringstream tgz_name;
    tgz_name << "firecracker-" << version << "-" << arch << ".tgz";
    const std::string tgz_path = fs::Join(*fc_dir, tgz_name.str());
    const std::string cth_path = fs::Join(*fc_dir, "cpu-template-helper");

    std::vector<std::pair<std::string, std::string> > vars;
    vars.push_back(std::make_pair(std::string("version"), version));
    vars.push_back(std::make_pair(std::string("arch"), arch));

    const core::Expected<Unit, std::string> downloaded =
        DownloadFile(ResolveUrl(url_template, vars), tgz_path);
    if (!downloaded.ok()) return downloaded;

    const core::Expected<Unit, std::string> extracted =
        ExtractFirecracker(tgz_path, fc_path, cth_path);
    if (!extracted.ok()) return extracted;

    // The tarball is only needed during extraction; dropping it keeps
    // `--setup-only` container layers small.
    fs::RemoveFile(tgz_path);
    return Unit();
}

core::Expected<Unit, std::string> EnsureKernel(const cfg::AppConfig& config,
                                               const Manifest& manifest,
                                               const std::string& arch) {
    if (config.kernel.image_path.has_value()) {
        // A kernel image is not executable, hence `false` here.
        return ValidateExplicitFile("kernel.image_path", *config.kernel.image_path, false);
    }

    const std::string kernel_path = config.ResolvedKernelImagePath();
    if (FileExistsNonEmpty(kernel_path)) {
        AGENTENV_DEBUG("kernel image already present path=" << kernel_path);
        return Unit();
    }

    const ManifestDownload& mode_manifest = manifest.kernel.ForMode(config.virtualization_mode);
    const std::string version =
        config.kernel.version.has_value() ? *config.kernel.version : mode_manifest.version;
    const std::string url_template =
        config.kernel.url.has_value() ? *config.kernel.url : mode_manifest.url;

    std::vector<std::pair<std::string, std::string> > vars;
    vars.push_back(std::make_pair(std::string("version"), version));
    vars.push_back(std::make_pair(std::string("arch"), arch));
    return DownloadFile(ResolveUrl(url_template, vars), kernel_path);
}

core::Expected<Unit, std::string> EnsureRegctl(const std::string& deps_path,
                                               const Manifest& manifest) {
    const std::string dest = cfg::RegctlPath(deps_path);

    // Probing the installed binary is cheaper and more reliable than trusting
    // a marker file, and tolerates a manual replacement.
    std::vector<std::string> version_argv;
    version_argv.push_back(dest);
    version_argv.push_back("version");
    const core::Expected<process::Output, std::string> probe = process::Run(version_argv);
    if (probe.ok() && probe.value().success() &&
        VersionOutputMentionsExactToken(probe.value().stdout_text, manifest.regctl.version)) {
        AGENTENV_DEBUG("regctl already installed, skipping path="
                       << dest << " version=" << manifest.regctl.version);
        return Unit();
    }

    // regctl publishes Go-style arch names, not uname ones.
    const core::Expected<std::string, std::string> host_arch = DetectArch();
    if (!host_arch.ok()) {
        return core::make_unexpected(std::string("unsupported architecture for regctl: ") +
                                     host_arch.error());
    }
    const std::string arch = host_arch.value() == "x86_64" ? "amd64" : "arm64";

    std::vector<std::pair<std::string, std::string> > vars;
    vars.push_back(std::make_pair(std::string("version"), manifest.regctl.version));
    vars.push_back(std::make_pair(std::string("arch"), arch));
    const std::string url = ResolveUrl(manifest.regctl.url, vars);

    AGENTENV_INFO("downloading regctl url=" << url << " dest=" << dest);

    // A stale binary must be removed first: `DownloadFile` skips a non-empty
    // destination, so the wrong version would survive otherwise.
    if (fs::Exists(dest)) {
        const core::Expected<Unit, std::string> removed = fs::RemoveFile(dest);
        if (!removed.ok()) {
            return core::make_unexpected(std::string("remove stale regctl binary ") + dest +
                                         ": " + removed.error());
        }
    }

    const core::Expected<Unit, std::string> downloaded = DownloadExecutableFile(url, dest);
    if (!downloaded.ok()) return downloaded;

    AGENTENV_INFO("regctl installed path=" << dest);
    return Unit();
}

core::Expected<Unit, std::string> VerifyInstalledToolsDrive(const std::string& source,
                                                            const std::string& destination,
                                                            const std::string& version) {
    const core::Expected<std::string, std::string> source_bytes = fs::ReadToString(source);
    if (!source_bytes.ok()) {
        return core::make_unexpected(std::string("hash tools drive source ") + source + ": " +
                                     source_bytes.error());
    }
    const core::Expected<std::string, std::string> installed_bytes =
        fs::ReadToString(destination);
    if (!installed_bytes.ok()) {
        return core::make_unexpected(std::string("hash installed tools drive ") + destination +
                                     ": " + installed_bytes.error());
    }

    if (core::Sha256Hex(source_bytes.value()) != core::Sha256Hex(installed_bytes.value())) {
        std::ostringstream oss;
        oss << "tools drive version '" << version << "' from " << source
            << " conflicts with the installed file at " << destination
            << "; publish a new version";
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

core::Expected<Unit, std::string> InstallExplicitToolsDrive(const std::string& source,
                                                            const std::string& destination,
                                                            const std::string& version) {
    const core::Expected<Unit, std::string> valid =
        ValidateExplicitFile("tools.drive_path", source, false);
    if (!valid.ok()) return valid;

    if (fs::Exists(destination)) {
        return VerifyInstalledToolsDrive(source, destination, version);
    }

    const core::Optional<std::string> parent = fs::Parent(destination);
    if (!parent.has_value()) {
        return core::make_unexpected(std::string("resolved tools drive path has no parent"));
    }
    const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
    if (!made.ok()) return made;

    const core::Expected<fs::TempFile, std::string> temporary =
        fs::CreateTempFileIn(*parent, ".tools-");
    if (!temporary.ok()) {
        return core::make_unexpected(std::string("create temporary tools drive in ") + *parent +
                                     ": " + temporary.error());
    }

    const core::Expected<Unit, std::string> copied =
        fs::Copy(source, temporary.value().path);
    if (!copied.ok()) {
        fs::RemoveFile(temporary.value().path);
        std::ostringstream oss;
        oss << "copy tools drive source " << source << " to " << temporary.value().path << ": "
            << copied.error();
        return core::make_unexpected(oss.str());
    }
    const core::Expected<Unit, std::string> mode =
        SetFileMode(temporary.value().path, 0644);
    if (!mode.ok()) {
        fs::RemoveFile(temporary.value().path);
        return mode;
    }

    // No-clobber: a concurrent installer may have published first, in which
    // case the two copies must agree rather than one overwriting the other.
    const core::Expected<bool, std::string> persisted =
        fs::PersistNoClobber(temporary.value().path, destination);
    if (!persisted.ok()) {
        fs::RemoveFile(temporary.value().path);
        return core::make_unexpected(std::string("install tools drive at ") + destination +
                                     ": " + persisted.error());
    }
    if (!persisted.value()) {
        return VerifyInstalledToolsDrive(source, destination, version);
    }

    AGENTENV_INFO("installed local tools drive version=" << version
                                                         << " path=" << destination);
    return Unit();
}

core::Expected<Unit, std::string> EnsureTools(const cfg::AppConfig& config) {
    // A drive or URL without a version would leave the release unidentifiable.
    if (!config.tools.version.has_value() &&
        (config.tools.drive_path.has_value() || config.tools.url.has_value())) {
        return core::make_unexpected(
            std::string("tools.version is required when tools.drive_path or tools.url is "
                        "configured"));
    }

    const core::Expected<std::string, std::string> tools_path = config.ResolvedToolsDrivePath();
    if (!tools_path.ok()) return core::make_unexpected(tools_path.error());

    if (config.tools.drive_path.has_value()) {
        return InstallExplicitToolsDrive(*config.tools.drive_path, tools_path.value(),
                                         config.ResolvedToolsVersion());
    }
    return Unit();
}

core::Expected<Unit, std::string> ExtractExt4FromGhcr(const std::string& regctl_path,
                                                      const std::string& image,
                                                      const std::string& filename,
                                                      const std::string& destination) {
    if (!FileExistsNonEmpty(regctl_path)) {
        return core::make_unexpected(
            std::string("regctl is required to download container images: ") + regctl_path);
    }
    if (!process::Exists("umoci")) {
        return core::make_unexpected(
            std::string("umoci is required to unpack container images"));
    }

    const core::Expected<std::string, std::string> tmp_dir =
        fs::CreateTempDir("agentenv-ghcr-");
    if (!tmp_dir.ok()) {
        return core::make_unexpected(std::string("failed to create temporary directory: ") +
                                     tmp_dir.error());
    }

    const std::string oci_dir = fs::Join(tmp_dir.value(), "oci");
    const std::string bundle_dir = fs::Join(tmp_dir.value(), "bundle");

    core::Expected<Unit, std::string> result = Unit();
    do {
        // `--platform local` is required: umoci can only unpack a
        // single-platform manifest.
        AGENTENV_INFO("pulling from container registry via regctl image=" << image
                                                                          << " filename="
                                                                          << filename);
        std::vector<std::string> regctl_argv;
        regctl_argv.push_back(regctl_path);
        regctl_argv.push_back("image");
        regctl_argv.push_back("copy");
        regctl_argv.push_back("--platform");
        regctl_argv.push_back("local");
        regctl_argv.push_back(image);
        regctl_argv.push_back("ocidir://" + oci_dir + ":latest");

        const core::Expected<process::Output, std::string> pulled = process::Run(regctl_argv);
        if (!pulled.ok() || !pulled.value().success()) {
            std::ostringstream oss;
            oss << "regctl image copy failed";
            if (pulled.ok() && !pulled.value().stderr_text.empty()) {
                oss << ": " << pulled.value().stderr_text;
            } else if (!pulled.ok()) {
                oss << ": " << pulled.error();
            }
            result = core::make_unexpected(oss.str());
            break;
        }

        std::vector<std::string> umoci_argv;
        umoci_argv.push_back("umoci");
        umoci_argv.push_back("unpack");
        umoci_argv.push_back("--rootless");
        umoci_argv.push_back("--image");
        umoci_argv.push_back(oci_dir + ":latest");
        umoci_argv.push_back(bundle_dir);

        const core::Expected<process::Output, std::string> unpacked = process::Run(umoci_argv);
        if (!unpacked.ok() || !unpacked.value().success()) {
            std::ostringstream oss;
            oss << "umoci unpack failed";
            if (unpacked.ok() && !unpacked.value().stderr_text.empty()) {
                oss << ": " << unpacked.value().stderr_text;
            } else if (!unpacked.ok()) {
                oss << ": " << unpacked.error();
            }
            result = core::make_unexpected(oss.str());
            break;
        }

        const std::string rootfs_dir = fs::Join(bundle_dir, "rootfs");
        std::string source = fs::Join(rootfs_dir, filename);
        if (!fs::IsFile(source)) {
            // Not at the root: search, since the image layout is the
            // platform's choice.
            source.clear();
            const core::Expected<std::vector<std::string>, std::string> entries =
                WalkDir(rootfs_dir);
            if (entries.ok()) {
                for (std::size_t i = 0; i < entries.value().size(); ++i) {
                    const core::Optional<std::string> name =
                        fs::FileName(entries.value()[i]);
                    if (name.has_value() && *name == filename) {
                        source = entries.value()[i];
                        break;
                    }
                }
            }
            if (source.empty()) {
                std::ostringstream oss;
                oss << "failed to find /" << filename << " in image " << image;
                result = core::make_unexpected(oss.str());
                break;
            }
        }

        // Rename first (same filesystem when TMPDIR allows), fall back to a
        // copy across devices.
        if (::rename(source.c_str(), destination.c_str()) != 0) {
            const core::Expected<Unit, std::string> copied = fs::Copy(source, destination);
            if (!copied.ok()) {
                result = core::make_unexpected(copied.error());
                break;
            }
        }
        result = SetFileMode(destination, 0644);
        if (result.ok()) {
            AGENTENV_INFO("extracted " << filename << " path=" << destination);
        }
    } while (false);

    fs::RemoveDirAll(tmp_dir.value());
    return result;
}

// ---------------------------------------------------------------------------
// Generated overlaybd global configs
// ---------------------------------------------------------------------------

core::Optional<std::string> DetectDockerCredentialConfig() {
    const char* docker_config = ::getenv("DOCKER_CONFIG");
    if (docker_config != NULL && docker_config[0] != '\0') {
        const std::string candidate = fs::Join(docker_config, "config.json");
        if (fs::IsFile(candidate)) return core::Optional<std::string>(candidate);
    }

    const char* home = ::getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        const std::string candidate = fs::Join(home, ".docker/config.json");
        if (fs::IsFile(candidate)) return core::Optional<std::string>(candidate);
    }
    return core::Optional<std::string>();
}

void OverlaybdCredentialFields(const core::Optional<std::string>& credential_path,
                               std::string* credential_file_path,
                               core::Json* credential_config) {
    core::JsonObject config;
    if (credential_path.has_value()) {
        *credential_file_path = *credential_path;
        config["mode"] = core::Json(std::string("file"));
        config["path"] = core::Json(*credential_path);
        config["timeout"] = core::Json(static_cast<int64_t>(5));
    } else {
        // The empty state is not all-zero: upstream writes a timeout of 1, and
        // the overlaybd runtime distinguishes the two.
        credential_file_path->clear();
        config["mode"] = core::Json(std::string(""));
        config["path"] = core::Json(std::string(""));
        config["timeout"] = core::Json(static_cast<int64_t>(1));
    }
    *credential_config = core::Json(config);
}

core::Expected<core::Json, std::string> OverlaybdRuntimeOssConfig(
    const cfg::OssBackendConfig& oss) {
    core::credentials::CredentialFields fields;
    fields.access_key_id = oss.access_key_id;
    fields.secret_access_key = oss.access_key_secret;
    fields.security_token = oss.security_token;
    fields.credential_process = oss.credential_process;

    core::credentials::CredentialSourceOptions options;
    options.scope = "backend.oss";
    options.allow_anonymous = false;
    options.required_access_key_id_label = "backend.oss.access_key_id";
    options.required_secret_access_key_label = "backend.oss.access_key_secret";

    const core::Expected<core::credentials::CredentialSource, std::string> source =
        core::credentials::FromFields(fields, options);
    if (!source.ok()) return core::make_unexpected(source.error());

    const core::Optional<std::string> region =
        core::credentials::Normalized(oss.region);
    if (!region.has_value()) {
        return core::make_unexpected(
            std::string("backend.oss.region must be set when generating overlaybd OSS config"));
    }

    std::string addressing_style;  // empty means auto-detect per endpoint
    if (oss.addressing_style.has_value()) {
        addressing_style =
            *oss.addressing_style == cfg::OssAddressingStyle::Path ? "path" : "virtual";
    }

    core::JsonObject config;
    config["enable"] = core::Json(true);
    config["defaultRegion"] = core::Json(*region);
    config["defaultEndpoint"] = core::Json(Trim(oss.endpoint));
    config["defaultAddressingStyle"] = core::Json(addressing_style);

    switch (source.value().kind) {
        case core::credentials::CredentialSource::Kind::Static: {
            const core::credentials::ResolvedCredential& credential =
                source.value().credential;
            config["accessKeyId"] = core::Json(credential.access_key_id);
            config["secretAccessKey"] = core::Json(credential.secret_access_key);
            config["securityToken"] = core::Json(
                credential.security_token.has_value() ? *credential.security_token
                                                      : std::string());
            config["credentialProcess"] = core::Json(std::string(""));
            break;
        }
        case core::credentials::CredentialSource::Kind::Process:
            // All static fields are written empty so the runtime cannot mix
            // the two mechanisms.
            config["accessKeyId"] = core::Json(std::string(""));
            config["secretAccessKey"] = core::Json(std::string(""));
            config["securityToken"] = core::Json(std::string(""));
            config["credentialProcess"] = core::Json(source.value().command);
            break;
        case core::credentials::CredentialSource::Kind::Anonymous:
            return core::make_unexpected(
                std::string("backend.oss requires non-anonymous credentials"));
    }
    return core::Json(config);
}

core::Expected<Unit, std::string> WriteGeneratedOverlaybdGlobalConfig(
    const std::string& path, const cfg::AppConfig& config,
    const cfg::ResolvedImageCacheConfig& image_cache,
    const core::Optional<std::string>& p2p_facade_address,
    const storage::overlaybd::DownloadConfig& download) {
    const core::Optional<std::string> parent = fs::Parent(path);
    const std::string config_dir = parent.has_value() ? *parent : std::string(".");
    const std::string log_path = fs::Join(config_dir, "overlaybd.log");

    const core::Optional<std::string> credential_path = DetectDockerCredentialConfig();
    if (credential_path.has_value()) {
        AGENTENV_INFO("found docker credential file; wiring overlaybd runtime to reuse it for "
                      "registry auth");
    }
    std::string credential_file_path;
    core::Json credential_config;
    OverlaybdCredentialFields(credential_path, &credential_file_path, &credential_config);

    core::JsonObject p2p;
    if (p2p_facade_address.has_value()) {
        p2p["enable"] = core::Json(true);
        p2p["address"] = core::Json(*p2p_facade_address);
    } else {
        p2p["enable"] = core::Json(false);
        p2p["address"] = core::Json(std::string("http://localhost:9731/accelerator"));
    }

    core::JsonObject log_config;
    log_config["logLevel"] = core::Json(static_cast<int64_t>(1));
    log_config["logPath"] = core::Json(log_path);

    core::JsonObject cache_config;
    cache_config["cacheType"] = core::Json(std::string("file"));
    cache_config["cacheDir"] = core::Json(image_cache.remote_blocks_dir);
    cache_config["cacheSizeGB"] =
        core::Json(static_cast<int64_t>(image_cache.remote_blocks_size_gb));
    cache_config["refillSize"] = core::Json(static_cast<int64_t>(262144));
    cache_config["blockSize"] = core::Json(static_cast<int64_t>(65536));

    core::JsonObject root;
    root["logConfig"] = core::Json(log_config);
    root["cacheConfig"] = core::Json(cache_config);
    root["credentialFilePath"] = core::Json(credential_file_path);
    root["credentialConfig"] = credential_config;
    root["ioEngine"] = core::Json(static_cast<int64_t>(0));
    root["download"] = storage::overlaybd::DownloadConfigToJson(download);
    root["p2pConfig"] = core::Json(p2p);
    root["enableAudit"] = core::Json(false);
    root["registryFsVersion"] = core::Json(std::string("v2"));
    root["remoteIoWorkers"] =
        core::Json(static_cast<int64_t>(config.ublk.overlaybd.remote_io_workers));

    if (config.snapshot.repository_backend == cfg::SnapshotRepositoryBackendKind::Oss) {
        if (!config.backend.oss.has_value()) {
            return core::make_unexpected(
                std::string("backend.oss config is required when "
                            "snapshot.repository_backend = oss"));
        }
        const core::Expected<core::Json, std::string> oss_config =
            OverlaybdRuntimeOssConfig(*config.backend.oss);
        if (!oss_config.ok()) return core::make_unexpected(oss_config.error());
        root["ossConfig"] = oss_config.value();
    }

    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create config parent dir ") + *parent +
                                         ": " + made.error());
        }
    }

    const core::Expected<Unit, std::string> written =
        fs::Write(path, core::Json(root).ToString() + "\n");
    if (!written.ok()) {
        return core::make_unexpected(std::string("write overlaybd global config ") + path +
                                     ": " + written.error());
    }
    // 0600: this file can embed static OSS credentials.
    return SetFileMode(path, 0600);
}

core::Expected<Unit, std::string> WriteGeneratedOverlaybdGlobalConfigs(
    const cfg::AppConfig& config, const core::Optional<std::string>& p2p_facade_address) {
    const cfg::ResolvedImageCacheConfig image_cache = config.ImageCacheLayout();

    storage::overlaybd::DownloadConfig rootfs_download;
    rootfs_download.enable = config.ublk.overlaybd.download_enable;

    // The memory snapshot path has its own tuned background-download policy.
    storage::overlaybd::DownloadConfig memory_download;
    memory_download.enable = config.memory_snapshot.background_download.enable;
    memory_download.delay = config.memory_snapshot.background_download.delay;
    memory_download.delay_extra = config.memory_snapshot.background_download.delay_extra;
    memory_download.try_cnt = config.memory_snapshot.background_download.try_cnt;
    memory_download.block_size = config.memory_snapshot.background_download.block_size;
    memory_download.concurrency = config.memory_snapshot.background_download.concurrency;
    memory_download.max_inflight_blocks =
        config.memory_snapshot.background_download.max_inflight_blocks;

    struct Target {
        std::string path;
        std::string blocks_subdir;  // empty keeps the shared remote-blocks dir
        const storage::overlaybd::DownloadConfig* download;
    };

    const storage::overlaybd::DownloadConfig offline_download;  // defaults: disabled

    std::vector<Target> targets;
    Target rootfs;
    rootfs.path = config.ublk.overlaybd.global_config_path;
    rootfs.download = &rootfs_download;
    targets.push_back(rootfs);

    Target memory;
    memory.path = config.memory_snapshot.overlaybd_global_config_path;
    memory.blocks_subdir = "memory-blocks";
    memory.download = &memory_download;
    targets.push_back(memory);

    // The offline C++ tools get isolated cache dirs: their file cache evicts
    // every file under cacheDir by truncate+unlink, which would destroy the
    // Rust runtime cache's per-entry directories if shared.
    Target convert;
    convert.path = config.ResolvedOverlaybdConvertGlobalConfigPath();
    convert.blocks_subdir = "convert-blocks";
    convert.download = &offline_download;
    targets.push_back(convert);

    Target resize;
    resize.path = config.ResolvedOverlaybdResizeGlobalConfigPath();
    resize.blocks_subdir = "resize-blocks";
    resize.download = &offline_download;
    targets.push_back(resize);

    for (std::size_t i = 0; i < targets.size(); ++i) {
        cfg::ResolvedImageCacheConfig cache = image_cache;
        if (!targets[i].blocks_subdir.empty()) {
            cache.remote_blocks_dir =
                fs::Join(image_cache.root_dir, targets[i].blocks_subdir);
        }
        const core::Expected<Unit, std::string> written = WriteGeneratedOverlaybdGlobalConfig(
            targets[i].path, config, cache, p2p_facade_address, *targets[i].download);
        if (!written.ok()) {
            return core::make_unexpected(std::string("write overlaybd global config ") +
                                         targets[i].path + ": " + written.error());
        }
    }
    return Unit();
}

}  // namespace deps
}  // namespace setup
}  // namespace agentenv
