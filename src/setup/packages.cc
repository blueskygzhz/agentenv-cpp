// SPDX-License-Identifier: MIT
// Rust: src/setup/packages.rs
#include "agentenv/setup/packages.h"

#include <algorithm>
#include <set>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/core/toml.h"

namespace agentenv {
namespace setup {
namespace packages {
namespace {

using core::Optional;
using core::Unit;
namespace process = core::process;

/// Rust `cmd_success` — status only, output discarded.
bool CmdSuccess(const std::vector<std::string>& argv) {
    const core::Expected<process::Output, std::string> output = process::Run(argv);
    return output.ok() && output.value().success();
}

/// Rust `cmd_stdout` — stdout on success, nothing otherwise.
Optional<std::string> CmdStdout(const std::vector<std::string>& argv) {
    const core::Expected<process::Output, std::string> output = process::Run(argv);
    if (!output.ok() || !output.value().success()) return Optional<std::string>();
    return Optional<std::string>(output.value().stdout_text);
}

/// Rust `sudo_cmd` — runs `argv` directly when already root, otherwise through
/// sudo. A failure to spawn counts as a failed command, not an error.
bool SudoCmd(const std::vector<std::string>& argv) {
    if (argv.empty()) return true;  // Rust returns Ok(true) for an empty command

    std::vector<std::string> final_argv;
    if (core::fs::EffectiveUidIsRoot()) {
        final_argv = argv;
    } else {
        final_argv.push_back("sudo");
        for (std::size_t i = 0; i < argv.size(); ++i) final_argv.push_back(argv[i]);
    }
    const core::Expected<int, std::string> status = process::Status(final_argv);
    return status.ok() && status.value() == 0;
}

std::vector<std::string> Argv(const char* a, const char* b = NULL, const char* c = NULL) {
    std::vector<std::string> argv;
    argv.push_back(a);
    if (b != NULL) argv.push_back(b);
    if (c != NULL) argv.push_back(c);
    return argv;
}

std::string ToLower(const std::string& value) {
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

/// Rust `trim_matches('"')` — strips quotes from both ends.
std::string TrimQuotes(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && value[begin] == '"') ++begin;
    while (end > begin && value[end - 1] == '"') --end;
    return value.substr(begin, end - begin);
}

std::string Join(const std::vector<std::string>& parts, const char* separator) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) oss << separator;
        oss << parts[i];
    }
    return oss.str();
}

/// Rust `install_sudo_packages`.
core::Expected<Unit, std::string> InstallSudoPackages(const char* manager,
                                                      const std::vector<std::string>& prefix,
                                                      const std::vector<std::string>& pkgs) {
    std::vector<std::string> argv = prefix;
    for (std::size_t i = 0; i < pkgs.size(); ++i) argv.push_back(pkgs[i]);
    if (!SudoCmd(argv)) {
        std::ostringstream oss;
        oss << "failed to install " << Join(pkgs, " ") << " via " << manager;
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

/// Rust `refresh_package_index`.
core::Expected<Unit, std::string> RefreshPackageIndex(PackageManager manager) {
    // Only apt needs an explicit refresh; the others either refresh as part of
    // installing or probe repository metadata directly.
    if (manager != PackageManager::Apt) return Unit();
    if (!SudoCmd(AptUpdateArgs())) {
        return core::make_unexpected(std::string("apt-get update failed"));
    }
    return Unit();
}

/// Rust `install_packages`.
core::Expected<Unit, std::string> InstallPackages(PackageManager manager,
                                                  const MissingRuntimeInstallPlan& plan) {
    if (!plan.unavailable.empty()) {
        return core::make_unexpected(MissingUnavailableRequirementsMessage(plan.unavailable));
    }
    if (plan.installable.empty()) return Unit();

    AGENTENV_INFO("installing missing runtime packages packages="
                  << Join(plan.installable, ","));

    switch (manager) {
        case PackageManager::Apt:
            return InstallSudoPackages("apt-get", AptInstallArgs(), plan.installable);
        case PackageManager::Dnf:
            return InstallSudoPackages("dnf", Argv("dnf", "install", "-y"), plan.installable);
        case PackageManager::Yum:
            return InstallSudoPackages("yum", Argv("yum", "install", "-y"), plan.installable);
        case PackageManager::Pacman: {
            std::vector<std::string> prefix = Argv("pacman", "-Sy", "--needed");
            prefix.push_back("--noconfirm");
            return InstallSudoPackages("pacman", prefix, plan.installable);
        }
        case PackageManager::Yay: {
            // yay refuses to run as root, so it is invoked directly rather
            // than through sudo.
            std::vector<std::string> argv = Argv("yay", "-S", "--needed");
            argv.push_back("--noconfirm");
            for (std::size_t i = 0; i < plan.installable.size(); ++i) {
                argv.push_back(plan.installable[i]);
            }
            const core::Expected<int, std::string> status = process::Status(argv);
            if (!status.ok() || status.value() != 0) {
                std::ostringstream oss;
                oss << "failed to install " << Join(plan.installable, " ") << " via yay";
                return core::make_unexpected(oss.str());
            }
            return Unit();
        }
    }
    return Unit();
}

/// Reads an optional string field out of a parsed inline table.
Optional<std::string> InlineField(const core::TomlTable& table, const std::string& prefix,
                                  const std::string& name, const char* field) {
    const core::TomlValue* value = table.Find(prefix + "." + name + "." + field);
    if (value == NULL) return Optional<std::string>();
    const core::Expected<std::string, std::string> text = value->AsString();
    if (!text.ok()) return Optional<std::string>();
    return Optional<std::string>(text.value());
}

std::vector<std::string> StringArrayOrEmpty(const core::TomlTable& table,
                                            const std::string& key) {
    const core::TomlValue* value = table.Find(key);
    if (value == NULL) return std::vector<std::string>();
    const core::Expected<std::vector<std::string>, std::string> array = value->AsStringArray();
    if (!array.ok()) return std::vector<std::string>();
    return array.value();
}

}  // namespace

std::vector<std::string> AptUpdateArgs() {
    std::vector<std::string> argv;
    argv.push_back("apt-get");
    argv.push_back("-o");
    argv.push_back("DPkg::Lock::Timeout=120");
    argv.push_back("-o");
    argv.push_back("APT::Get::Lock-Timeout=120");
    argv.push_back("update");
    return argv;
}

std::vector<std::string> AptInstallArgs() {
    std::vector<std::string> argv;
    argv.push_back("apt-get");
    argv.push_back("-o");
    argv.push_back("DPkg::Lock::Timeout=120");
    argv.push_back("-o");
    argv.push_back("APT::Get::Lock-Timeout=120");
    argv.push_back("install");
    argv.push_back("-y");
    return argv;
}

const char* DistroToString(Distro distro) {
    switch (distro) {
        case Distro::Ubuntu:
            return "ubuntu";
        case Distro::Debian:
            return "debian";
        case Distro::Centos:
            return "centos";
        case Distro::Rhel:
            return "rhel";
        case Distro::Arch:
            return "arch";
    }
    return "unknown";
}

std::ostream& operator<<(std::ostream& os, Distro distro) { return os << DistroToString(distro); }

const char* PackageManagerToString(PackageManager manager) {
    switch (manager) {
        case PackageManager::Apt:
            return "apt";
        case PackageManager::Dnf:
            return "dnf";
        case PackageManager::Yum:
            return "yum";
        case PackageManager::Pacman:
            return "pacman";
        case PackageManager::Yay:
            return "yay";
    }
    return "unknown";
}

std::ostream& operator<<(std::ostream& os, PackageManager manager) {
    return os << PackageManagerToString(manager);
}

Optional<Distro> DistroFromId(const std::string& id) {
    if (id == "ubuntu") return Optional<Distro>(Distro::Ubuntu);
    if (id == "debian") return Optional<Distro>(Distro::Debian);
    // These all ship RHEL-compatible package names but identify as themselves.
    if (id == "centos" || id == "centos-stream" || id == "tencentos" || id == "openeuler") {
        return Optional<Distro>(Distro::Centos);
    }
    if (id == "rhel" || id == "redhat" || id == "redhatenterpriseserver") {
        return Optional<Distro>(Distro::Rhel);
    }
    if (id == "arch" || id == "manjaro") return Optional<Distro>(Distro::Arch);
    return Optional<Distro>();
}

Optional<Distro> OsReleaseDistro(const std::string& os_release) {
    Optional<std::string> id;
    std::vector<std::string> id_like;

    std::istringstream stream(os_release);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);

        if (line.compare(0, 3, "ID=") == 0) {
            id = ToLower(TrimQuotes(line.substr(3)));
        } else if (line.compare(0, 8, "ID_LIKE=") == 0) {
            std::istringstream likes(TrimQuotes(line.substr(8)));
            std::string like;
            while (likes >> like) id_like.push_back(ToLower(like));
        }
    }

    // `ID` wins; `ID_LIKE` is the fallback that lets derivatives (Rocky,
    // AlmaLinux) resolve to their upstream.
    if (id.has_value()) {
        const Optional<Distro> direct = DistroFromId(*id);
        if (direct.has_value()) return direct;
    }
    for (std::size_t i = 0; i < id_like.size(); ++i) {
        const Optional<Distro> like = DistroFromId(id_like[i]);
        if (like.has_value()) return like;
    }
    return Optional<Distro>();
}

core::Expected<Distro, std::string> DetectDistro() {
    const core::Expected<std::string, std::string> os_release =
        core::fs::ReadToString("/etc/os-release");
    if (!os_release.ok()) {
        return core::make_unexpected(std::string("read /etc/os-release: ") + os_release.error());
    }
    const Optional<Distro> distro = OsReleaseDistro(os_release.value());
    if (!distro.has_value()) {
        return core::make_unexpected(std::string("unsupported Linux distribution"));
    }
    return *distro;
}

core::Expected<PackageManager, std::string> DetectPackageManager(Distro distro) {
    // Candidate order is significant: yay (AUR-capable) before pacman, dnf
    // before yum.
    std::vector<std::pair<std::string, PackageManager> > candidates;
    switch (distro) {
        case Distro::Ubuntu:
        case Distro::Debian:
            candidates.push_back(std::make_pair(std::string("apt-get"), PackageManager::Apt));
            break;
        case Distro::Centos:
        case Distro::Rhel:
            candidates.push_back(std::make_pair(std::string("dnf"), PackageManager::Dnf));
            candidates.push_back(std::make_pair(std::string("yum"), PackageManager::Yum));
            break;
        case Distro::Arch:
            candidates.push_back(std::make_pair(std::string("yay"), PackageManager::Yay));
            candidates.push_back(std::make_pair(std::string("pacman"), PackageManager::Pacman));
            break;
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (process::Exists(candidates[i].first)) return candidates[i].second;
    }
    return core::make_unexpected(std::string("no supported package manager found"));
}

PackageChoice PackageChoice::Parse(const std::string& package) {
    // `a|b` means either satisfies the requirement. Note that Rust's `split`
    // keeps empty segments, so "a|" yields ["a", ""] — reproduced here.
    PackageChoice choice;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t bar = package.find('|', cursor);
        if (bar == std::string::npos) {
            choice.candidates.push_back(package.substr(cursor));
            break;
        }
        choice.candidates.push_back(package.substr(cursor, bar - cursor));
        cursor = bar + 1;
    }
    return choice;
}

std::string PackageChoice::ToString() const {
    if (candidates.empty()) return "<empty package entry>";
    if (candidates.size() == 1) return candidates[0];
    return Join(candidates, " or ");
}

Optional<std::string> RuntimePackageByDistro::PackageForDistro(Distro distro) const {
    const Optional<std::string>* specific = NULL;
    switch (distro) {
        case Distro::Ubuntu:
            specific = &ubuntu;
            break;
        case Distro::Debian:
            specific = &debian;
            break;
        case Distro::Centos:
            specific = &centos;
            break;
        case Distro::Rhel:
            specific = &rhel;
            break;
        case Distro::Arch:
            specific = &arch;
            break;
    }
    if (specific != NULL && specific->has_value()) return *specific;
    return default_package;
}

const std::vector<std::string>& RuntimePackages::ForDistro(Distro distro) const {
    switch (distro) {
        case Distro::Ubuntu:
            return ubuntu;
        case Distro::Debian:
            return debian;
        case Distro::Centos:
            return centos;
        case Distro::Rhel:
            return rhel;
        case Distro::Arch:
            return arch;
    }
    return common;
}

core::Expected<Manifest, std::string> Manifest::ParseString(const std::string& toml_text) {
    const core::Expected<core::TomlTable, std::string> parsed =
        core::TomlTable::ParseString(toml_text);
    if (!parsed.ok()) return core::make_unexpected(parsed.error());
    const core::TomlTable& table = parsed.value();

    Manifest manifest;
    manifest.runtime.common = StringArrayOrEmpty(table, "packages.runtime.common");
    manifest.runtime.ubuntu = StringArrayOrEmpty(table, "packages.runtime.ubuntu");
    manifest.runtime.debian = StringArrayOrEmpty(table, "packages.runtime.debian");
    manifest.runtime.centos = StringArrayOrEmpty(table, "packages.runtime.centos");
    manifest.runtime.rhel = StringArrayOrEmpty(table, "packages.runtime.rhel");
    manifest.runtime.arch = StringArrayOrEmpty(table, "packages.runtime.arch");

    // `InlineTableKeys` is what makes a command named `mkfs.ext4` recoverable;
    // its flattened key is indistinguishable from a nested table otherwise.
    const std::string prefix = "packages.runtime_commands";
    const std::vector<std::string> commands = table.InlineTableKeys(prefix);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        RuntimePackageByDistro by_distro;
        by_distro.default_package = InlineField(table, prefix, commands[i], "default");
        by_distro.ubuntu = InlineField(table, prefix, commands[i], "ubuntu");
        by_distro.debian = InlineField(table, prefix, commands[i], "debian");
        by_distro.centos = InlineField(table, prefix, commands[i], "centos");
        by_distro.rhel = InlineField(table, prefix, commands[i], "rhel");
        by_distro.arch = InlineField(table, prefix, commands[i], "arch");
        manifest.runtime_commands.push_back(std::make_pair(commands[i], by_distro));
    }
    return manifest;
}

core::Expected<Manifest, std::string> Manifest::ParseFile(const std::string& path) {
    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    if (!text.ok()) {
        return core::make_unexpected(std::string("read dependency manifest ") + path + ": " +
                                     text.error());
    }
    return ParseString(text.value());
}

MissingRuntimeRequirement MissingRuntimeRequirement::OfPackage(const PackageChoice& choice) {
    MissingRuntimeRequirement requirement;
    requirement.kind = Kind::Package;
    requirement.package = choice;
    return requirement;
}

MissingRuntimeRequirement MissingRuntimeRequirement::OfCommand(
    const std::string& command, const Optional<std::string>& package) {
    MissingRuntimeRequirement requirement;
    requirement.kind = Kind::Command;
    requirement.command = command;
    if (package.has_value()) requirement.package = PackageChoice::Parse(*package);
    return requirement;
}

std::string MissingRuntimeRequirement::ToString() const {
    std::ostringstream oss;
    if (kind == Kind::Package) {
        oss << "package " << (package.has_value() ? package->ToString() : "<empty package entry>");
        return oss.str();
    }
    oss << "command " << command << " (provided by "
        << (package.has_value() ? package->ToString() : "<unknown package>") << ")";
    return oss.str();
}

bool MissingRuntimeRequirement::operator==(const MissingRuntimeRequirement& o) const {
    if (kind != o.kind || command != o.command) return false;
    if (package.has_value() != o.package.has_value()) return false;
    return !package.has_value() || *package == *o.package;
}

std::ostream& operator<<(std::ostream& os, const MissingRuntimeRequirement& requirement) {
    return os << requirement.ToString();
}

bool CommandPresent(const std::string& command) { return process::Exists(command); }

bool PackageInstalled(PackageManager manager, const std::string& package) {
    switch (manager) {
        case PackageManager::Apt: {
            std::vector<std::string> argv =
                Argv("dpkg-query", "-W", "-f=${Status}");
            argv.push_back(package);
            const Optional<std::string> status = CmdStdout(argv);
            // dpkg-query exits 0 for a removed-but-known package, so the
            // status text is what actually decides.
            return status.has_value() &&
                   status->find("install ok installed") != std::string::npos;
        }
        case PackageManager::Dnf:
        case PackageManager::Yum: {
            std::vector<std::string> argv = Argv("rpm", "-q");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
        case PackageManager::Pacman:
        case PackageManager::Yay: {
            std::vector<std::string> argv = Argv("pacman", "-Qi");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
    }
    return false;
}

bool PackageAvailable(PackageManager manager, const std::string& package) {
    switch (manager) {
        case PackageManager::Apt: {
            std::vector<std::string> argv = Argv("apt-cache", "show");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
        case PackageManager::Dnf: {
            std::vector<std::string> argv = Argv("dnf", "list", "--available");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
        case PackageManager::Yum: {
            std::vector<std::string> argv = Argv("yum", "list", "--available");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
        case PackageManager::Pacman: {
            std::vector<std::string> argv = Argv("pacman", "-Si");
            argv.push_back(package);
            return CmdSuccess(argv);
        }
        case PackageManager::Yay: {
            // AUR first, then the official repositories.
            std::vector<std::string> yay = Argv("yay", "-Si");
            yay.push_back(package);
            if (CmdSuccess(yay)) return true;
            std::vector<std::string> pacman = Argv("pacman", "-Si");
            pacman.push_back(package);
            return CmdSuccess(pacman);
        }
    }
    return false;
}

std::vector<MissingRuntimeRequirement> MissingRuntimeRequirementsForDistro(
    PackageManager manager, const Manifest& manifest, Distro distro) {
    std::vector<MissingRuntimeRequirement> missing;

    // Package-only requirements: `common` then the distro's own list.
    std::vector<std::string> package_names = manifest.runtime.common;
    const std::vector<std::string>& distro_packages = manifest.runtime.ForDistro(distro);
    for (std::size_t i = 0; i < distro_packages.size(); ++i) {
        package_names.push_back(distro_packages[i]);
    }

    for (std::size_t i = 0; i < package_names.size(); ++i) {
        const PackageChoice choice = PackageChoice::Parse(package_names[i]);
        bool satisfied = false;
        for (std::size_t c = 0; c < choice.candidates.size(); ++c) {
            if (PackageInstalled(manager, choice.candidates[c])) {
                satisfied = true;
                break;
            }
        }
        if (!satisfied) missing.push_back(MissingRuntimeRequirement::OfPackage(choice));
    }

    // Command requirements are probed through PATH, which is both cheaper and
    // more accurate than asking the package manager.
    for (std::size_t i = 0; i < manifest.runtime_commands.size(); ++i) {
        const std::string& command = manifest.runtime_commands[i].first;
        if (CommandPresent(command)) continue;
        missing.push_back(MissingRuntimeRequirement::OfCommand(
            command, manifest.runtime_commands[i].second.PackageForDistro(distro)));
    }
    return missing;
}

MissingRuntimeInstallPlan SelectInstallPackagesWithProbe(
    PackageManager manager, const std::vector<MissingRuntimeRequirement>& requirements,
    AvailabilityProbe is_package_available) {
    // A set, so the plan is deduplicated and sorted like Rust's BTreeSet —
    // several requirements can resolve to the same package.
    std::set<std::string> installable;
    MissingRuntimeInstallPlan plan;

    for (std::size_t i = 0; i < requirements.size(); ++i) {
        const Optional<PackageChoice>& choice = requirements[i].PackageChoiceRef();
        if (!choice.has_value()) {
            plan.unavailable.push_back(requirements[i]);
            continue;
        }

        bool selected = false;
        for (std::size_t c = 0; c < choice->candidates.size(); ++c) {
            if (is_package_available(manager, choice->candidates[c])) {
                installable.insert(choice->candidates[c]);
                selected = true;
                break;  // first available candidate wins
            }
        }
        if (!selected) plan.unavailable.push_back(requirements[i]);
    }

    plan.installable.assign(installable.begin(), installable.end());
    return plan;
}

std::string FormatRequirements(const std::vector<MissingRuntimeRequirement>& requirements) {
    std::vector<std::string> rendered;
    rendered.reserve(requirements.size());
    for (std::size_t i = 0; i < requirements.size(); ++i) {
        rendered.push_back(requirements[i].ToString());
    }
    return Join(rendered, ", ");
}

std::string MissingUnavailableRequirementsMessage(
    const std::vector<MissingRuntimeRequirement>& unavailable) {
    std::ostringstream oss;
    oss << "AENV setup could not find packages for these required runtime requirements in the "
           "configured package repositories: "
        << FormatRequirements(unavailable)
        << ". Enable the appropriate distro repositories or install equivalent packages that "
           "provide the required commands/libraries, then rerun setup.";
    return oss.str();
}

core::Expected<Unit, std::string> Ensure(const std::string& manifest_path) {
    const core::Expected<Distro, std::string> distro = DetectDistro();
    if (!distro.ok()) return core::make_unexpected(distro.error());

    const core::Expected<PackageManager, std::string> manager =
        DetectPackageManager(distro.value());
    if (!manager.ok()) return core::make_unexpected(manager.error());

    const core::Expected<Manifest, std::string> manifest = Manifest::ParseFile(manifest_path);
    if (!manifest.ok()) return core::make_unexpected(manifest.error());

    const std::vector<MissingRuntimeRequirement> missing =
        MissingRuntimeRequirementsForDistro(manager.value(), manifest.value(), distro.value());
    if (missing.empty()) return Unit();

    // The index refresh only happens when something is actually missing.
    const core::Expected<Unit, std::string> refreshed = RefreshPackageIndex(manager.value());
    if (!refreshed.ok()) return refreshed;

    const MissingRuntimeInstallPlan plan =
        SelectInstallPackagesWithProbe(manager.value(), missing, PackageAvailable);
    return InstallPackages(manager.value(), plan);
}

core::Expected<Unit, std::string> CheckRuntime(const std::string& manifest_path) {
    const core::Expected<Distro, std::string> distro = DetectDistro();
    if (!distro.ok()) return core::make_unexpected(distro.error());

    const core::Expected<PackageManager, std::string> manager =
        DetectPackageManager(distro.value());
    if (!manager.ok()) return core::make_unexpected(manager.error());

    const core::Expected<Manifest, std::string> manifest = Manifest::ParseFile(manifest_path);
    if (!manifest.ok()) return core::make_unexpected(manifest.error());

    const std::vector<MissingRuntimeRequirement> missing =
        MissingRuntimeRequirementsForDistro(manager.value(), manifest.value(), distro.value());
    if (!missing.empty()) {
        std::ostringstream oss;
        oss << "missing AENV runtime requirements: " << FormatRequirements(missing)
            << "; run `server --setup-only` or install the listed packages manually";
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

}  // namespace packages
}  // namespace setup
}  // namespace agentenv
