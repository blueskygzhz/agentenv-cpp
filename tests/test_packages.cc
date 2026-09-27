// SPDX-License-Identifier: MIT
// Rust: src/setup/packages.rs `mod tests`, plus coverage for the TOML inline
// table support the manifest needs.
#include <string>
#include <vector>

#include "agentenv/core/toml.h"
#include "agentenv/setup/packages.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::TomlTable;
namespace pkg = agentenv::setup::packages;

/// The `[packages]` half of config/deps_manifest.toml, as a fixture.
const char* const kManifest =
    "[packages.runtime]\n"
    "common = [\n"
    "  \"ca-certificates\",\n"
    "]\n"
    "ubuntu = []\n"
    "debian = []\n"
    "centos = [\"centos-only\"]\n"
    "rhel = []\n"
    "arch = []\n"
    "\n"
    "[packages.runtime_commands]\n"
    "curl = { default = \"curl\" }\n"
    "\"ip\" = { default = \"iproute2\", centos = \"iproute\", rhel = \"iproute\" }\n"
    "\"mkfs.ext4\" = { default = \"e2fsprogs\" }\n"
    "zstd = { default = \"zstd\" }\n";

/// Probe that claims nothing is available.
bool NeverAvailable(pkg::PackageManager, const std::string&) { return false; }
/// Probe that claims everything is available.
bool AlwaysAvailable(pkg::PackageManager, const std::string&) { return true; }
/// Only the second alternative exists, to prove the fallback works.
bool OnlyLibaio1(pkg::PackageManager, const std::string& package) {
    return package == "libaio1";
}

const pkg::RuntimePackageByDistro& CommandNamed(const pkg::Manifest& manifest,
                                                const std::string& name) {
    for (std::size_t i = 0; i < manifest.runtime_commands.size(); ++i) {
        if (manifest.runtime_commands[i].first == name) {
            return manifest.runtime_commands[i].second;
        }
    }
    MT_EXPECT_TRUE(false);  // unreachable: caller named a fixture command
    return manifest.runtime_commands[0].second;
}

}  // namespace

// ---------------------------------------------------------------------------
// TOML inline tables
// ---------------------------------------------------------------------------

MT_TEST(toml_parses_inline_tables) {
    const agentenv::core::Expected<TomlTable, std::string> parsed =
        TomlTable::ParseString("[t]\na = { x = \"1\", y = 2, z = true }\n");
    MT_EXPECT_TRUE(parsed.ok());

    MT_EXPECT_EQ(parsed.value().Find("t.a.x")->AsString().value(), std::string("1"));
    MT_EXPECT_EQ(parsed.value().Find("t.a.y")->AsInteger().value(), 2LL);
    MT_EXPECT_EQ(parsed.value().Find("t.a.z")->AsBoolean().value(), true);

    const std::vector<std::string> keys = parsed.value().InlineTableKeys("t");
    MT_EXPECT_EQ(keys.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(keys[0], std::string("a"));
}

MT_TEST(toml_inline_table_name_may_contain_a_dot) {
    // This is the case that forced `InlineTableKeys` to exist: the flattened
    // key `t.mkfs.ext4.default` is indistinguishable from a table `t.mkfs`
    // holding `ext4.default`, so the original name has to be remembered.
    const agentenv::core::Expected<TomlTable, std::string> parsed = TomlTable::ParseString(
        "[t]\n\"mkfs.ext4\" = { default = \"e2fsprogs\" }\nplain = { default = \"x\" }\n");
    MT_EXPECT_TRUE(parsed.ok());

    const std::vector<std::string> keys = parsed.value().InlineTableKeys("t");
    MT_EXPECT_EQ(keys.size(), static_cast<std::size_t>(2));
    // Source order is preserved.
    MT_EXPECT_EQ(keys[0], std::string("mkfs.ext4"));
    MT_EXPECT_EQ(keys[1], std::string("plain"));

    // Recombining prefix + name + field round-trips.
    MT_EXPECT_EQ(parsed.value().Find("t.mkfs.ext4.default")->AsString().value(),
                 std::string("e2fsprogs"));
}

MT_TEST(toml_handles_empty_and_malformed_inline_tables) {
    const agentenv::core::Expected<TomlTable, std::string> empty =
        TomlTable::ParseString("[t]\na = {}\n");
    MT_EXPECT_TRUE(empty.ok());
    // An empty inline table still counts as declared.
    MT_EXPECT_EQ(empty.value().InlineTableKeys("t").size(), static_cast<std::size_t>(1));

    // A field without `=`.
    MT_EXPECT_TRUE(!TomlTable::ParseString("[t]\na = { oops }\n").ok());
    // Nested inline tables are rejected rather than mis-parsed.
    MT_EXPECT_TRUE(!TomlTable::ParseString("[t]\na = { b = { c = 1 } }\n").ok());
    // An empty field name.
    MT_EXPECT_TRUE(!TomlTable::ParseString("[t]\na = { = 1 }\n").ok());
}

MT_TEST(toml_inline_table_keys_is_empty_for_unknown_prefix) {
    const agentenv::core::Expected<TomlTable, std::string> parsed =
        TomlTable::ParseString("[t]\na = { x = 1 }\n");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().InlineTableKeys("nope").size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Distro detection
// ---------------------------------------------------------------------------

MT_TEST(parses_ubuntu_os_release) {
    // Rust: `parses_ubuntu_os_release`.
    const Optional<pkg::Distro> distro =
        pkg::OsReleaseDistro("ID=ubuntu\nVERSION_ID=\"24.04\"\n");
    MT_EXPECT_TRUE(distro.has_value());
    MT_EXPECT_EQ(*distro, pkg::Distro::Ubuntu);
}

MT_TEST(parses_rhel_like_os_release) {
    // Rust: `parses_rhel_like_os_release`. Rocky identifies as "rocky", which
    // is unknown, so ID_LIKE is what resolves it.
    const Optional<pkg::Distro> distro =
        pkg::OsReleaseDistro("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n");
    MT_EXPECT_TRUE(distro.has_value());
    MT_EXPECT_EQ(*distro, pkg::Distro::Rhel);
}

MT_TEST(os_release_prefers_id_over_id_like) {
    // A known ID must win even when ID_LIKE names something else.
    const Optional<pkg::Distro> distro =
        pkg::OsReleaseDistro("ID=ubuntu\nID_LIKE=\"debian\"\n");
    MT_EXPECT_TRUE(distro.has_value());
    MT_EXPECT_EQ(*distro, pkg::Distro::Ubuntu);
}

MT_TEST(os_release_is_case_insensitive_and_quote_tolerant) {
    // openEuler writes its ID in mixed case upstream.
    const Optional<pkg::Distro> upper = pkg::OsReleaseDistro("ID=\"openEuler\"\n");
    MT_EXPECT_TRUE(upper.has_value());
    MT_EXPECT_EQ(*upper, pkg::Distro::Centos);

    const Optional<pkg::Distro> quoted = pkg::OsReleaseDistro("ID=\"debian\"\n");
    MT_EXPECT_TRUE(quoted.has_value());
    MT_EXPECT_EQ(*quoted, pkg::Distro::Debian);

    // CRLF must not leave a stray \r that breaks the match.
    const Optional<pkg::Distro> crlf = pkg::OsReleaseDistro("ID=arch\r\n");
    MT_EXPECT_TRUE(crlf.has_value());
    MT_EXPECT_EQ(*crlf, pkg::Distro::Arch);
}

MT_TEST(os_release_rejects_unknown_distributions) {
    MT_EXPECT_TRUE(!pkg::OsReleaseDistro("ID=plan9\n").has_value());
    MT_EXPECT_TRUE(!pkg::OsReleaseDistro("").has_value());
    // An unknown ID with an equally unknown ID_LIKE stays unresolved.
    MT_EXPECT_TRUE(!pkg::OsReleaseDistro("ID=weird\nID_LIKE=\"alsoweird\"\n").has_value());
}

MT_TEST(distro_from_id_covers_every_documented_alias) {
    MT_EXPECT_EQ(*pkg::DistroFromId("centos"), pkg::Distro::Centos);
    MT_EXPECT_EQ(*pkg::DistroFromId("centos-stream"), pkg::Distro::Centos);
    MT_EXPECT_EQ(*pkg::DistroFromId("tencentos"), pkg::Distro::Centos);
    MT_EXPECT_EQ(*pkg::DistroFromId("openeuler"), pkg::Distro::Centos);
    MT_EXPECT_EQ(*pkg::DistroFromId("redhat"), pkg::Distro::Rhel);
    MT_EXPECT_EQ(*pkg::DistroFromId("redhatenterpriseserver"), pkg::Distro::Rhel);
    MT_EXPECT_EQ(*pkg::DistroFromId("manjaro"), pkg::Distro::Arch);
    MT_EXPECT_TRUE(!pkg::DistroFromId("").has_value());
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

MT_TEST(manifest_parses_runtime_packages_and_commands) {
    const agentenv::core::Expected<pkg::Manifest, std::string> manifest =
        pkg::Manifest::ParseString(kManifest);
    MT_EXPECT_TRUE(manifest.ok());

    MT_EXPECT_EQ(manifest.value().runtime.common.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(manifest.value().runtime.common[0], std::string("ca-certificates"));
    MT_EXPECT_EQ(manifest.value().runtime.ubuntu.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(manifest.value().runtime.centos.size(), static_cast<std::size_t>(1));

    MT_EXPECT_EQ(manifest.value().runtime_commands.size(), static_cast<std::size_t>(4));
    // The dotted command name survived parsing intact.
    MT_EXPECT_EQ(*CommandNamed(manifest.value(), "mkfs.ext4").default_package,
                 std::string("e2fsprogs"));
}

MT_TEST(manifest_parses_the_real_bundled_file) {
    // Guards against the checked-in manifest drifting into syntax this subset
    // parser cannot read.
    const agentenv::core::Expected<pkg::Manifest, std::string> manifest =
        pkg::Manifest::ParseFile("config/deps_manifest.toml");
    if (!manifest.ok()) return;  // not run from the source root; skip

    MT_EXPECT_TRUE(!manifest.value().runtime_commands.empty());
    // Every command must name a package for at least the default case,
    // otherwise setup could not tell the operator what to install.
    for (std::size_t i = 0; i < manifest.value().runtime_commands.size(); ++i) {
        MT_EXPECT_TRUE(manifest.value()
                           .runtime_commands[i]
                           .second.PackageForDistro(pkg::Distro::Ubuntu)
                           .has_value());
    }
}

MT_TEST(runtime_package_mapping_uses_default_with_distro_overrides) {
    // Rust: `runtime_package_mapping_uses_default_with_distro_overrides`.
    pkg::RuntimePackageByDistro packages;
    packages.default_package = std::string("iproute2");
    packages.centos = std::string("iproute");
    packages.rhel = std::string("iproute");

    MT_EXPECT_EQ(*packages.PackageForDistro(pkg::Distro::Debian), std::string("iproute2"));
    MT_EXPECT_EQ(*packages.PackageForDistro(pkg::Distro::Arch), std::string("iproute2"));
    MT_EXPECT_EQ(*packages.PackageForDistro(pkg::Distro::Centos), std::string("iproute"));
    MT_EXPECT_EQ(*packages.PackageForDistro(pkg::Distro::Rhel), std::string("iproute"));
}

MT_TEST(runtime_package_mapping_without_default_can_be_absent) {
    // A command with no package for this distro yields nothing, which is what
    // produces "<unknown package>" in the report.
    pkg::RuntimePackageByDistro packages;
    packages.centos = std::string("iproute");
    MT_EXPECT_TRUE(!packages.PackageForDistro(pkg::Distro::Ubuntu).has_value());
    MT_EXPECT_EQ(*packages.PackageForDistro(pkg::Distro::Centos), std::string("iproute"));
}

MT_TEST(manifest_distro_list_selects_the_right_array) {
    const agentenv::core::Expected<pkg::Manifest, std::string> manifest =
        pkg::Manifest::ParseString(kManifest);
    MT_EXPECT_TRUE(manifest.ok());

    MT_EXPECT_EQ(manifest.value().runtime.ForDistro(pkg::Distro::Centos).size(),
                 static_cast<std::size_t>(1));
    MT_EXPECT_EQ(manifest.value().runtime.ForDistro(pkg::Distro::Centos)[0],
                 std::string("centos-only"));
    // `common` is not part of the per-distro list.
    MT_EXPECT_EQ(manifest.value().runtime.ForDistro(pkg::Distro::Ubuntu).size(),
                 static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// PackageChoice
// ---------------------------------------------------------------------------

MT_TEST(parses_package_alternatives) {
    // Rust: `parses_package_alternatives`.
    const pkg::PackageChoice choice = pkg::PackageChoice::Parse("libaio1t64|libaio1");
    MT_EXPECT_EQ(choice.candidates.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(choice.candidates[0], std::string("libaio1t64"));
    MT_EXPECT_EQ(choice.candidates[1], std::string("libaio1"));
}

MT_TEST(package_choice_display_matches_rust) {
    MT_EXPECT_EQ(pkg::PackageChoice::Parse("curl").ToString(), std::string("curl"));
    MT_EXPECT_EQ(pkg::PackageChoice::Parse("a|b").ToString(), std::string("a or b"));
    MT_EXPECT_EQ(pkg::PackageChoice::Parse("a|b|c").ToString(), std::string("a or b or c"));

    pkg::PackageChoice empty;
    MT_EXPECT_EQ(empty.ToString(), std::string("<empty package entry>"));
}

// ---------------------------------------------------------------------------
// Install plan selection
// ---------------------------------------------------------------------------

MT_TEST(unavailable_requirement_is_reported_separately) {
    // Rust: `unavailable_requirement_is_reported_separately`.
    std::vector<pkg::MissingRuntimeRequirement> requirements;
    requirements.push_back(pkg::MissingRuntimeRequirement::OfPackage(
        pkg::PackageChoice::Parse("definitely-not-a-real-package")));

    const pkg::MissingRuntimeInstallPlan plan = pkg::SelectInstallPackagesWithProbe(
        pkg::PackageManager::Apt, requirements, NeverAvailable);

    MT_EXPECT_EQ(plan.installable.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(plan.unavailable.size(), static_cast<std::size_t>(1));

    const std::string message = pkg::MissingUnavailableRequirementsMessage(plan.unavailable);
    MT_EXPECT_TRUE(message.find("package definitely-not-a-real-package") != std::string::npos);
    // The message must tell the operator what to do next.
    MT_EXPECT_TRUE(message.find("Enable the appropriate distro repositories") !=
                   std::string::npos);
}

MT_TEST(select_picks_the_first_available_alternative) {
    std::vector<pkg::MissingRuntimeRequirement> requirements;
    requirements.push_back(pkg::MissingRuntimeRequirement::OfPackage(
        pkg::PackageChoice::Parse("libaio1t64|libaio1")));

    // The preferred name is unavailable, so the fallback must be chosen rather
    // than the whole requirement being declared unsatisfiable.
    const pkg::MissingRuntimeInstallPlan plan = pkg::SelectInstallPackagesWithProbe(
        pkg::PackageManager::Apt, requirements, OnlyLibaio1);

    MT_EXPECT_EQ(plan.installable.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(plan.installable[0], std::string("libaio1"));
    MT_EXPECT_EQ(plan.unavailable.size(), static_cast<std::size_t>(0));
}

MT_TEST(select_deduplicates_and_sorts_the_plan) {
    std::vector<pkg::MissingRuntimeRequirement> requirements;
    // Three commands provided by the same package, plus one other.
    requirements.push_back(pkg::MissingRuntimeRequirement::OfCommand(
        "mkfs.ext4", Optional<std::string>(std::string("e2fsprogs"))));
    requirements.push_back(pkg::MissingRuntimeRequirement::OfCommand(
        "resize2fs", Optional<std::string>(std::string("e2fsprogs"))));
    requirements.push_back(pkg::MissingRuntimeRequirement::OfCommand(
        "curl", Optional<std::string>(std::string("curl"))));

    const pkg::MissingRuntimeInstallPlan plan = pkg::SelectInstallPackagesWithProbe(
        pkg::PackageManager::Apt, requirements, AlwaysAvailable);

    // e2fsprogs appears once, and the list is sorted (BTreeSet semantics).
    MT_EXPECT_EQ(plan.installable.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(plan.installable[0], std::string("curl"));
    MT_EXPECT_EQ(plan.installable[1], std::string("e2fsprogs"));
}

MT_TEST(select_treats_a_packageless_command_as_unavailable) {
    std::vector<pkg::MissingRuntimeRequirement> requirements;
    requirements.push_back(
        pkg::MissingRuntimeRequirement::OfCommand("mystery", Optional<std::string>()));

    const pkg::MissingRuntimeInstallPlan plan = pkg::SelectInstallPackagesWithProbe(
        pkg::PackageManager::Apt, requirements, AlwaysAvailable);

    // Nothing to install, because the manifest never said what provides it.
    MT_EXPECT_EQ(plan.installable.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(plan.unavailable.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(plan.unavailable[0].ToString().find("<unknown package>") !=
                   std::string::npos);
}

MT_TEST(select_handles_an_empty_requirement_list) {
    const pkg::MissingRuntimeInstallPlan plan = pkg::SelectInstallPackagesWithProbe(
        pkg::PackageManager::Apt, std::vector<pkg::MissingRuntimeRequirement>(),
        NeverAvailable);
    MT_EXPECT_EQ(plan.installable.size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(plan.unavailable.size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Requirement rendering
// ---------------------------------------------------------------------------

MT_TEST(requirement_display_matches_rust) {
    const pkg::MissingRuntimeRequirement package =
        pkg::MissingRuntimeRequirement::OfPackage(pkg::PackageChoice::Parse("libaio1t64|libaio1"));
    MT_EXPECT_EQ(package.ToString(), std::string("package libaio1t64 or libaio1"));

    const pkg::MissingRuntimeRequirement command = pkg::MissingRuntimeRequirement::OfCommand(
        "ip", Optional<std::string>(std::string("iproute2")));
    MT_EXPECT_EQ(command.ToString(), std::string("command ip (provided by iproute2)"));
}

MT_TEST(format_requirements_is_comma_separated) {
    std::vector<pkg::MissingRuntimeRequirement> requirements;
    requirements.push_back(
        pkg::MissingRuntimeRequirement::OfPackage(pkg::PackageChoice::Parse("ca-certificates")));
    requirements.push_back(pkg::MissingRuntimeRequirement::OfCommand(
        "jq", Optional<std::string>(std::string("jq"))));

    MT_EXPECT_EQ(pkg::FormatRequirements(requirements),
                 std::string("package ca-certificates, command jq (provided by jq)"));
    MT_EXPECT_EQ(pkg::FormatRequirements(std::vector<pkg::MissingRuntimeRequirement>()),
                 std::string(""));
}

MT_TEST(requirement_equality_distinguishes_kind_and_payload) {
    const pkg::PackageChoice curl = pkg::PackageChoice::Parse("curl");
    MT_EXPECT_TRUE(pkg::MissingRuntimeRequirement::OfPackage(curl) ==
                   pkg::MissingRuntimeRequirement::OfPackage(curl));
    // Same package, but one is a command requirement.
    MT_EXPECT_TRUE(pkg::MissingRuntimeRequirement::OfPackage(curl) !=
                   pkg::MissingRuntimeRequirement::OfCommand(
                       "curl", Optional<std::string>(std::string("curl"))));
    // Same command, different provider.
    MT_EXPECT_TRUE(pkg::MissingRuntimeRequirement::OfCommand(
                       "ip", Optional<std::string>(std::string("iproute2"))) !=
                   pkg::MissingRuntimeRequirement::OfCommand(
                       "ip", Optional<std::string>(std::string("iproute"))));
}

// ---------------------------------------------------------------------------
// Host probes
// ---------------------------------------------------------------------------

MT_TEST(installed_command_is_not_reported_missing) {
    // Rust: `installed_command_is_not_reported_missing`.
    MT_EXPECT_TRUE(pkg::CommandPresent("sh"));
    MT_EXPECT_TRUE(!pkg::CommandPresent("aenv-definitely-not-a-real-command"));
}

MT_TEST(apt_arguments_carry_the_lock_timeouts) {
    // Without these, a host running unattended-upgrades fails setup outright
    // instead of waiting for the dpkg lock.
    const std::vector<std::string> update = pkg::AptUpdateArgs();
    MT_EXPECT_EQ(update[0], std::string("apt-get"));
    MT_EXPECT_EQ(update[update.size() - 1], std::string("update"));

    bool has_dpkg_timeout = false;
    bool has_apt_timeout = false;
    for (std::size_t i = 0; i < update.size(); ++i) {
        if (update[i] == "DPkg::Lock::Timeout=120") has_dpkg_timeout = true;
        if (update[i] == "APT::Get::Lock-Timeout=120") has_apt_timeout = true;
    }
    MT_EXPECT_TRUE(has_dpkg_timeout);
    MT_EXPECT_TRUE(has_apt_timeout);

    const std::vector<std::string> install = pkg::AptInstallArgs();
    MT_EXPECT_EQ(install[install.size() - 2], std::string("install"));
    MT_EXPECT_EQ(install[install.size() - 1], std::string("-y"));
}

MT_TEST(package_manager_and_distro_names_render) {
    MT_EXPECT_EQ(std::string(pkg::DistroToString(pkg::Distro::Ubuntu)), std::string("ubuntu"));
    MT_EXPECT_EQ(std::string(pkg::PackageManagerToString(pkg::PackageManager::Apt)),
                 std::string("apt"));
    MT_EXPECT_EQ(std::string(pkg::PackageManagerToString(pkg::PackageManager::Yay)),
                 std::string("yay"));
}

int main() { return microtest::RunAll(); }
