// SPDX-License-Identifier: MIT
// Rust: src/setup/packages.rs
//
// Detects the host distribution and package manager, works out which runtime
// requirements are missing, and installs them.
//
// Porting note. Rust embeds `config/deps_manifest.toml` with `include_str!`,
// so the manifest is compiled in. C++11 has no equivalent, so the manifest is
// parsed from disk and the path is a parameter — which also makes the whole
// module testable against a fixture manifest rather than the host's.
#ifndef AGENTENV_SETUP_PACKAGES_H_
#define AGENTENV_SETUP_PACKAGES_H_

#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace setup {
namespace packages {

/// Rust `APT_UPDATE_ARGS` / `APT_INSTALL_ARGS`. The lock timeouts matter: on a
/// host where unattended-upgrades holds the dpkg lock, the default behaviour is
/// to fail immediately.
std::vector<std::string> AptUpdateArgs();
std::vector<std::string> AptInstallArgs();

/// Rust enum `Distro`.
enum class Distro {
    Ubuntu,
    Debian,
    Centos,
    Rhel,
    Arch,
};

const char* DistroToString(Distro distro);
std::ostream& operator<<(std::ostream& os, Distro distro);

/// Rust enum `PackageManager`.
enum class PackageManager {
    Apt,
    Dnf,
    Yum,
    Pacman,
    Yay,
};

const char* PackageManagerToString(PackageManager manager);
std::ostream& operator<<(std::ostream& os, PackageManager manager);

/// Rust `distro_from_id` — maps an `/etc/os-release` ID onto a known distro.
core::Optional<Distro> DistroFromId(const std::string& id);

/// Rust `os_release_distro` — prefers `ID`, then falls back to `ID_LIKE`, so
/// a derivative such as Rocky resolves through `ID_LIKE="rhel centos fedora"`.
core::Optional<Distro> OsReleaseDistro(const std::string& os_release);

/// Rust `detect_distro` — reads `/etc/os-release`.
core::Expected<Distro, std::string> DetectDistro();

/// Rust `detect_package_manager` — the first candidate present in `PATH` wins,
/// and the candidate order is per-distro (yay before pacman on Arch, dnf
/// before yum on RHEL).
core::Expected<PackageManager, std::string> DetectPackageManager(Distro distro);

/// Rust struct `PackageChoice` — alternatives written `a|b` in the manifest,
/// meaning "any one of these satisfies the requirement".
struct PackageChoice {
    std::vector<std::string> candidates;

    /// Rust `PackageChoice::from_str`.
    static PackageChoice Parse(const std::string& package);

    /// Rust `impl Display` — `a or b`, or `<empty package entry>`.
    std::string ToString() const;

    bool operator==(const PackageChoice& o) const { return candidates == o.candidates; }
    bool operator!=(const PackageChoice& o) const { return !(*this == o); }
};

/// Rust struct `RuntimePackageByDistro` — one command's package name, with
/// optional per-distro overrides.
struct RuntimePackageByDistro {
    core::Optional<std::string> default_package;
    core::Optional<std::string> ubuntu;
    core::Optional<std::string> debian;
    core::Optional<std::string> centos;
    core::Optional<std::string> rhel;
    core::Optional<std::string> arch;

    /// Rust `package_for_distro` — the distro-specific name if present,
    /// otherwise the default.
    core::Optional<std::string> PackageForDistro(Distro distro) const;
};

/// Rust struct `RuntimePackages` — package-only requirements per distro.
struct RuntimePackages {
    std::vector<std::string> common;
    std::vector<std::string> ubuntu;
    std::vector<std::string> debian;
    std::vector<std::string> centos;
    std::vector<std::string> rhel;
    std::vector<std::string> arch;

    /// The distro-specific list, excluding `common`.
    const std::vector<std::string>& ForDistro(Distro distro) const;
};

/// Rust struct `SetupDependencyManifest` (its `packages` half).
struct Manifest {
    RuntimePackages runtime;
    /// Rust `runtime_commands`: command name -> package that provides it.
    /// Ordered, because it drives the order of the missing-requirement report.
    std::vector<std::pair<std::string, RuntimePackageByDistro> > runtime_commands;

    /// Parses the `[packages]` section of `deps_manifest.toml`.
    static core::Expected<Manifest, std::string> ParseString(const std::string& toml_text);
    static core::Expected<Manifest, std::string> ParseFile(const std::string& path);
};

/// Rust enum `MissingRuntimeRequirement`.
struct MissingRuntimeRequirement {
    enum class Kind {
        /// Rust `Package(..)` — a library with no probe-able executable.
        Package,
        /// Rust `Command { command, package }` — probed via `PATH`.
        Command,
    };

    Kind kind = Kind::Package;
    /// Set for `Command`.
    std::string command;
    /// Always set for `Package`; may be absent for `Command` when the manifest
    /// has no package for this distro.
    core::Optional<PackageChoice> package;

    static MissingRuntimeRequirement OfPackage(const PackageChoice& choice);
    /// Rust `MissingRuntimeRequirement::command`.
    static MissingRuntimeRequirement OfCommand(const std::string& command,
                                               const core::Optional<std::string>& package);

    /// Rust `package_choice`.
    const core::Optional<PackageChoice>& PackageChoiceRef() const { return package; }

    /// Rust `impl Display` — `package X` or `command X (provided by Y)`.
    std::string ToString() const;

    bool operator==(const MissingRuntimeRequirement& o) const;
    bool operator!=(const MissingRuntimeRequirement& o) const { return !(*this == o); }
};

std::ostream& operator<<(std::ostream& os, const MissingRuntimeRequirement& requirement);

/// Rust struct `MissingRuntimeInstallPlan`.
struct MissingRuntimeInstallPlan {
    /// Deduplicated and sorted, as Rust's `BTreeSet` makes it.
    std::vector<std::string> installable;
    /// Requirements no configured repository can satisfy.
    std::vector<MissingRuntimeRequirement> unavailable;
};

/// Probe used to decide whether a package could be installed. Injectable so
/// the selection logic can be tested without a package manager; Rust does the
/// same via `select_install_packages_with_probe`.
typedef bool (*AvailabilityProbe)(PackageManager manager, const std::string& package);

/// Rust `select_install_packages_with_probe` — picks the first available
/// candidate of each choice, and records the rest as unavailable.
MissingRuntimeInstallPlan SelectInstallPackagesWithProbe(
    PackageManager manager, const std::vector<MissingRuntimeRequirement>& requirements,
    AvailabilityProbe is_package_available);

/// Rust `format_requirements` — comma-separated.
std::string FormatRequirements(const std::vector<MissingRuntimeRequirement>& requirements);

/// Rust `missing_unavailable_requirements_message`.
std::string MissingUnavailableRequirementsMessage(
    const std::vector<MissingRuntimeRequirement>& unavailable);

/// Rust `package_installed`.
bool PackageInstalled(PackageManager manager, const std::string& package);

/// Rust `package_available`.
bool PackageAvailable(PackageManager manager, const std::string& package);

/// Rust `command_present`.
bool CommandPresent(const std::string& command);

/// Rust `missing_runtime_requirements_for_distro`.
std::vector<MissingRuntimeRequirement> MissingRuntimeRequirementsForDistro(
    PackageManager manager, const Manifest& manifest, Distro distro);

/// Rust `ensure` — detect, diff against the manifest, install what is missing.
core::Expected<core::Unit, std::string> Ensure(const std::string& manifest_path);

/// Rust `check_runtime` — the same diff, but reporting instead of installing.
core::Expected<core::Unit, std::string> CheckRuntime(const std::string& manifest_path);

}  // namespace packages
}  // namespace setup
}  // namespace agentenv
#endif  // AGENTENV_SETUP_PACKAGES_H_
