// SPDX-License-Identifier: MIT
// Rust: src/setup/overlaybd.rs `mod tests`, plus coverage for the deps helpers
// it builds on.
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/process.h"
#include "agentenv/setup/deps.h"
#include "agentenv/setup/overlaybd.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
namespace fs = agentenv::core::fs;
namespace deps = agentenv::setup::deps;
namespace obd = agentenv::setup::overlaybd;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-obd-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value();
    }
    ~TempRoot() { fs::RemoveDirAll(path); }

    std::string Join(const std::string& leaf) const { return fs::Join(path, leaf); }

    void WriteFile(const std::string& relative, const std::string& content) const {
        const std::string full = fs::Join(path, relative);
        const Optional<std::string> parent = fs::Parent(full);
        MT_EXPECT_TRUE(parent.has_value());
        MT_EXPECT_TRUE(fs::CreateDirAll(*parent).ok());
        MT_EXPECT_TRUE(fs::Write(full, content).ok());
    }
};

/// Builds an extracted-release layout: all four tools plus the default config.
void WriteCompleteReleasePayload(const TempRoot& root, const std::string& prefix) {
    const std::vector<std::string>& tools = obd::ToolNames();
    for (std::size_t i = 0; i < tools.size(); ++i) {
        root.WriteFile(fs::Join(prefix, "bin/" + tools[i]), "static tool");
    }
    root.WriteFile(fs::Join(prefix, "etc/overlaybd/overlaybd.json"), "{}\n");
}

obd::DependencyConfig ConfigWith(const std::string& version, const std::string& package_url) {
    obd::DependencyConfig config;
    config.version = version;
    config.package_url = package_url;
    return config;
}

}  // namespace

// ---------------------------------------------------------------------------
// deps helpers
// ---------------------------------------------------------------------------

MT_TEST(detect_arch_reports_a_supported_architecture) {
    const agentenv::core::Expected<std::string, std::string> arch = deps::DetectArch();
    // The build only targets these two; anything else is a hard error.
    if (arch.ok()) {
        MT_EXPECT_TRUE(arch.value() == "x86_64" || arch.value() == "aarch64");
    } else {
        MT_EXPECT_TRUE(arch.error().find("unsupported architecture") != std::string::npos);
    }
}

MT_TEST(copy_file_creates_parents_and_can_mark_executable) {
    TempRoot root;
    root.WriteFile("source.bin", "payload");

    const std::string destination = root.Join("nested/deeper/target.bin");
    MT_EXPECT_TRUE(deps::CopyFile(root.Join("source.bin"), destination, true).ok());

    MT_EXPECT_EQ(fs::ReadToString(destination).value(), std::string("payload"));
    MT_EXPECT_EQ(fs::Stat(destination).value().mode, static_cast<uint32_t>(0755));

    // Without the executable flag the mode is left as the copy produced it.
    const std::string plain = root.Join("plain.bin");
    MT_EXPECT_TRUE(deps::CopyFile(root.Join("source.bin"), plain, false).ok());
    MT_EXPECT_TRUE((fs::Stat(plain).value().mode & 0111) == 0);
}

MT_TEST(file_exists_nonempty_treats_a_truncated_file_as_absent) {
    TempRoot root;
    // A zero-length file is what a failed earlier download leaves behind, so
    // it must not count as already-downloaded.
    root.WriteFile("empty", "");
    MT_EXPECT_TRUE(!deps::FileExistsNonEmpty(root.Join("empty")));

    root.WriteFile("full", "x");
    MT_EXPECT_TRUE(deps::FileExistsNonEmpty(root.Join("full")));

    MT_EXPECT_TRUE(!deps::FileExistsNonEmpty(root.Join("missing")));
    // A directory is not a file.
    MT_EXPECT_TRUE(fs::CreateDirAll(root.Join("dir")).ok());
    MT_EXPECT_TRUE(!deps::FileExistsNonEmpty(root.Join("dir")));
}

MT_TEST(download_skips_an_already_present_file) {
    TempRoot root;
    root.WriteFile("cached", "original");

    // The URL is unreachable; the call must still succeed by skipping.
    MT_EXPECT_TRUE(
        deps::DownloadFile("http://127.0.0.1:1/nope", root.Join("cached")).ok());
    MT_EXPECT_EQ(fs::ReadToString(root.Join("cached")).value(), std::string("original"));
}

MT_TEST(download_failure_leaves_no_partial_file) {
    TempRoot root;
    const std::string destination = root.Join("sub/target.bin");

    // Port 1 refuses connections, so curl fails.
    const agentenv::core::Expected<Unit, std::string> result =
        deps::DownloadFile("http://127.0.0.1:1/nope", destination);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("GET http://127.0.0.1:1/nope") != std::string::npos);

    // Neither the final name nor the temporary may survive a failure.
    MT_EXPECT_TRUE(!fs::Exists(destination));
    MT_EXPECT_TRUE(!fs::Exists(destination + ".tmp"));
}

MT_TEST(copy_dir_recursive_reproduces_the_tree) {
    TempRoot root;
    root.WriteFile("src/a.txt", "a");
    root.WriteFile("src/nested/b.txt", "b");
    root.WriteFile("src/nested/deeper/c.txt", "c");

    MT_EXPECT_TRUE(deps::CopyDirRecursive(root.Join("src"), root.Join("dst")).ok());

    MT_EXPECT_EQ(fs::ReadToString(root.Join("dst/a.txt")).value(), std::string("a"));
    MT_EXPECT_EQ(fs::ReadToString(root.Join("dst/nested/b.txt")).value(), std::string("b"));
    MT_EXPECT_EQ(fs::ReadToString(root.Join("dst/nested/deeper/c.txt")).value(),
                 std::string("c"));
}

MT_TEST(extract_tar_gz_round_trips_a_real_archive) {
    TempRoot root;
    root.WriteFile("payload/bin/overlaybd-create", "tool");
    root.WriteFile("payload/etc/overlaybd/overlaybd.json", "{}\n");

    // Build the archive with the same `tar` the extractor shells out to.
    std::vector<std::string> argv;
    argv.push_back("tar");
    argv.push_back("-czf");
    argv.push_back(root.Join("package.tar.gz"));
    argv.push_back("-C");
    argv.push_back(root.Join("payload"));
    argv.push_back(".");
    const agentenv::core::Expected<int, std::string> packed =
        agentenv::core::process::Status(argv);
    if (!packed.ok() || packed.value() != 0) return;  // no tar on this host

    MT_EXPECT_TRUE(
        deps::ExtractTarGz(root.Join("package.tar.gz"), root.Join("out")).ok());
    MT_EXPECT_EQ(fs::ReadToString(root.Join("out/bin/overlaybd-create")).value(),
                 std::string("tool"));
    MT_EXPECT_TRUE(fs::IsFile(root.Join("out/etc/overlaybd/overlaybd.json")));
}

MT_TEST(extract_tar_gz_reports_a_corrupt_archive) {
    TempRoot root;
    root.WriteFile("broken.tar.gz", "this is not gzip data");

    const agentenv::core::Expected<Unit, std::string> result =
        deps::ExtractTarGz(root.Join("broken.tar.gz"), root.Join("out"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("extract overlaybd package") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ConfiguredRelease
// ---------------------------------------------------------------------------

MT_TEST(configured_overlaybd_release_expands_arch_url) {
    // Rust: `configured_overlaybd_release_expands_arch_url`.
    const obd::DependencyConfig config = ConfigWith(
        "v1.0.18-aenv.1",
        "https://example.invalid/static-{version}/overlaybd-tools-{version}-linux-{arch}.tar.gz");

    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> release =
        obd::ConfiguredReleaseFor(config, "aarch64");
    MT_EXPECT_TRUE(release.ok());
    MT_EXPECT_EQ(release.value().tag_name, std::string("v1.0.18-aenv.1"));
    // Both `{version}` placeholders are expanded, not just the first.
    MT_EXPECT_EQ(release.value().package_url,
                 std::string("https://example.invalid/static-v1.0.18-aenv.1/"
                             "overlaybd-tools-v1.0.18-aenv.1-linux-aarch64.tar.gz"));
}

MT_TEST(configured_overlaybd_release_rejects_unsupported_architecture) {
    // Rust: `configured_overlaybd_release_rejects_unsupported_architecture`.
    const obd::DependencyConfig config =
        ConfigWith("v1.0.18-aenv.1", "https://example.invalid/{arch}.tar.gz");

    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> release =
        obd::ConfiguredReleaseFor(config, "riscv64");
    MT_EXPECT_TRUE(!release.ok());
    MT_EXPECT_TRUE(release.error().find("unsupported overlaybd release architecture") !=
                   std::string::npos);
}

MT_TEST(configured_overlaybd_release_validates_version_and_url) {
    // An empty or whitespace-only version is rejected.
    MT_EXPECT_TRUE(!obd::ConfiguredReleaseFor(ConfigWith("", "https://x/{arch}"), "x86_64").ok());
    MT_EXPECT_TRUE(
        !obd::ConfiguredReleaseFor(ConfigWith("  ", "https://x/{arch}"), "x86_64").ok());

    // No URL at all.
    obd::DependencyConfig no_url;
    no_url.version = "v1";
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> missing =
        obd::ConfiguredReleaseFor(no_url, "x86_64");
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().find("overlaybd.package_url not set") != std::string::npos);

    // An empty URL string is the same as none.
    MT_EXPECT_TRUE(!obd::ConfiguredReleaseFor(ConfigWith("v1", "   "), "x86_64").ok());
}

MT_TEST(configured_overlaybd_release_prefers_package_url_over_legacy_url) {
    obd::DependencyConfig config;
    config.version = "v1";
    config.url = std::string("https://legacy.invalid/{version}.tar.gz");
    config.package_url = std::string("https://current.invalid/{version}.tar.gz");

    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> release =
        obd::ConfiguredReleaseFor(config, "x86_64");
    MT_EXPECT_TRUE(release.ok());
    MT_EXPECT_EQ(release.value().package_url,
                 std::string("https://current.invalid/v1.tar.gz"));

    // With only the legacy field set, it is used as the fallback.
    obd::DependencyConfig legacy_only;
    legacy_only.version = "v1";
    legacy_only.url = std::string("https://legacy.invalid/{version}.tar.gz");
    MT_EXPECT_EQ(obd::ConfiguredReleaseFor(legacy_only, "x86_64").value().package_url,
                 std::string("https://legacy.invalid/v1.tar.gz"));
}

MT_TEST(pinned_converter_v1_constants_are_expandable) {
    // The pinned converter must itself produce a valid URL, otherwise
    // `EnsureToolsConverterV1` could never download anything.
    const obd::DependencyConfig config =
        ConfigWith(obd::kConverterV1Version, obd::kConverterV1PackageUrl);
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> release =
        obd::ConfiguredReleaseFor(config, "x86_64");
    MT_EXPECT_TRUE(release.ok());
    MT_EXPECT_TRUE(release.value().package_url.find("{version}") == std::string::npos);
    MT_EXPECT_TRUE(release.value().package_url.find("{arch}") == std::string::npos);
    MT_EXPECT_TRUE(release.value().package_url.find("v1.0.18-aenv.1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// InstalledRelease marker
// ---------------------------------------------------------------------------

MT_TEST(installed_release_json_round_trips) {
    obd::InstalledRelease release;
    release.tag_name = "v1.0.18-aenv.1";
    release.asset_name = "overlaybd-tools-v1.0.18-aenv.1-linux-x86_64.tar.gz";

    const agentenv::core::Expected<obd::InstalledRelease, std::string> parsed =
        obd::InstalledRelease::ParseJson(release.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value() == release);
    MT_EXPECT_TRUE(!parsed.value().digest.has_value());
}

MT_TEST(installed_release_parses_the_upstream_fixture_shape) {
    // Exactly the JSON the Rust test writes, including `"digest":null`.
    const agentenv::core::Expected<obd::InstalledRelease, std::string> parsed =
        obd::InstalledRelease::ParseJson(
            "{\n \"tag_name\":\"future-runtime-release\", \"asset_name\":\"other.tar.gz\", "
            "\"digest\":null\n}");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().tag_name, std::string("future-runtime-release"));
    MT_EXPECT_EQ(parsed.value().asset_name, std::string("other.tar.gz"));
    // A null digest is absent, not the string "null".
    MT_EXPECT_TRUE(!parsed.value().digest.has_value());
}

MT_TEST(installed_release_rejects_malformed_metadata) {
    MT_EXPECT_TRUE(!obd::InstalledRelease::ParseJson("not json").ok());
    MT_EXPECT_TRUE(!obd::InstalledRelease::ParseJson("[]").ok());
    // Missing required fields must fail rather than yield an empty tag, which
    // would make an unrelated install look current.
    MT_EXPECT_TRUE(!obd::InstalledRelease::ParseJson("{\"tag_name\":\"v1\"}").ok());
    MT_EXPECT_TRUE(!obd::InstalledRelease::ParseJson("{\"asset_name\":\"a.tar.gz\"}").ok());
}

MT_TEST(read_installed_release_reports_absence_as_empty) {
    TempRoot root;
    const agentenv::core::Expected<Optional<obd::InstalledRelease>, std::string> absent =
        obd::ReadInstalledRelease(root.Join("tools-release.json"));
    MT_EXPECT_TRUE(absent.ok());
    MT_EXPECT_TRUE(!absent.value().has_value());

    // A present but corrupt marker is an error, not an absence.
    root.WriteFile("tools-release.json", "garbage");
    MT_EXPECT_TRUE(!obd::ReadInstalledRelease(root.Join("tools-release.json")).ok());
}

MT_TEST(desired_installed_release_carries_no_digest) {
    obd::ConfiguredRelease configured;
    configured.tag_name = "v1";
    configured.package_url = "https://x/a.tar.gz";

    const obd::InstalledRelease desired =
        obd::DesiredInstalledRelease(configured, "a.tar.gz");
    MT_EXPECT_EQ(desired.tag_name, std::string("v1"));
    MT_EXPECT_EQ(desired.asset_name, std::string("a.tar.gz"));
    MT_EXPECT_TRUE(!desired.digest.has_value());
}

MT_TEST(installed_release_equality_distinguishes_every_field) {
    obd::InstalledRelease base;
    base.tag_name = "v1";
    base.asset_name = "a.tar.gz";

    obd::InstalledRelease other_tag = base;
    other_tag.tag_name = "v2";
    MT_EXPECT_TRUE(base != other_tag);

    obd::InstalledRelease other_asset = base;
    other_asset.asset_name = "b.tar.gz";
    MT_EXPECT_TRUE(base != other_asset);

    obd::InstalledRelease with_digest = base;
    with_digest.digest = std::string("sha256:abc");
    MT_EXPECT_TRUE(base != with_digest);
}

// ---------------------------------------------------------------------------
// Tool installation
// ---------------------------------------------------------------------------

MT_TEST(tools_present_requires_every_tool) {
    TempRoot root;
    const std::vector<std::string>& tools = obd::ToolNames();
    MT_EXPECT_EQ(tools.size(), static_cast<std::size_t>(4));

    MT_EXPECT_TRUE(!obd::ToolsPresent(root.Join("install")));

    // All but the last: still incomplete.
    for (std::size_t i = 0; i + 1 < tools.size(); ++i) {
        root.WriteFile("install/bin/" + tools[i], "tool");
    }
    MT_EXPECT_TRUE(!obd::ToolsPresent(root.Join("install")));

    root.WriteFile("install/bin/" + tools[tools.size() - 1], "tool");
    MT_EXPECT_TRUE(obd::ToolsPresent(root.Join("install")));
}

MT_TEST(installs_static_tools_without_lib_and_removes_legacy_lib) {
    // Rust: `installs_static_tools_without_lib_and_removes_legacy_lib`.
    TempRoot root;
    WriteCompleteReleasePayload(root, "extracted");
    // A legacy dynamic-library directory left by an older release.
    root.WriteFile("installed/lib/liblegacy.so", "legacy");

    MT_EXPECT_TRUE(
        obd::InstallReleaseTools(root.Join("extracted"), root.Join("installed")).ok());

    // The stale lib dir must be gone, or the loader could pick it up.
    MT_EXPECT_TRUE(!fs::Exists(root.Join("installed/lib")));

    const std::vector<std::string>& tools = obd::ToolNames();
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const std::string installed = root.Join("installed/bin/" + tools[i]);
        MT_EXPECT_TRUE(fs::IsFile(installed));
        MT_EXPECT_EQ(fs::Stat(installed).value().mode, static_cast<uint32_t>(0755));
    }
}

MT_TEST(install_release_tools_rejects_an_incomplete_payload) {
    TempRoot root;
    // bin/ exists but is missing three of the four tools.
    root.WriteFile("extracted/bin/overlaybd-create", "tool");

    const agentenv::core::Expected<Unit, std::string> result =
        obd::InstallReleaseTools(root.Join("extracted"), root.Join("installed"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("missing required tool") != std::string::npos);

    // A failed install must not leave a partial bin/ behind.
    MT_EXPECT_TRUE(!fs::Exists(root.Join("installed/bin")));
}

MT_TEST(install_release_tools_rejects_a_payload_without_bin) {
    TempRoot root;
    root.WriteFile("extracted/etc/overlaybd/overlaybd.json", "{}\n");

    const agentenv::core::Expected<Unit, std::string> result =
        obd::InstallReleaseTools(root.Join("extracted"), root.Join("installed"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("missing bin dir") != std::string::npos);
}

MT_TEST(install_release_tools_replaces_a_previous_install) {
    TempRoot root;
    WriteCompleteReleasePayload(root, "extracted");
    // A previous install with a stale extra binary.
    root.WriteFile("installed/bin/overlaybd-create", "old tool");
    root.WriteFile("installed/bin/stale-helper", "stale");

    MT_EXPECT_TRUE(
        obd::InstallReleaseTools(root.Join("extracted"), root.Join("installed")).ok());

    // The directory was swapped wholesale, so the stale file is gone and the
    // tool has the new content.
    MT_EXPECT_TRUE(!fs::Exists(root.Join("installed/bin/stale-helper")));
    MT_EXPECT_EQ(fs::ReadToString(root.Join("installed/bin/overlaybd-create")).value(),
                 std::string("static tool"));

    // No staging or backup directories are left behind.
    const agentenv::core::Expected<std::vector<std::string>, std::string> entries =
        fs::ReadDir(root.Join("installed"));
    MT_EXPECT_TRUE(entries.ok());
    for (std::size_t i = 0; i < entries.value().size(); ++i) {
        MT_EXPECT_TRUE(entries.value()[i].find("release-staging-") == std::string::npos);
        MT_EXPECT_TRUE(entries.value()[i].find("release-backup-") == std::string::npos);
    }
}

MT_TEST(stage_default_config_copies_the_packaged_file) {
    TempRoot root;
    root.WriteFile("extracted/etc/overlaybd/overlaybd.json", "{\"packaged\":true}\n");

    MT_EXPECT_TRUE(
        obd::StageDefaultConfig(root.Join("extracted"), root.Join("installed")).ok());
    MT_EXPECT_EQ(
        fs::ReadToString(root.Join("installed/etc/overlaybd/overlaybd.json")).value(),
        std::string("{\"packaged\":true}\n"));
}

MT_TEST(stage_default_config_rejects_a_payload_without_it) {
    TempRoot root;
    WriteCompleteReleasePayload(root, "extracted");
    MT_EXPECT_TRUE(fs::RemoveFile(root.Join("extracted/etc/overlaybd/overlaybd.json")).ok());

    const agentenv::core::Expected<Unit, std::string> result =
        obd::StageDefaultConfig(root.Join("extracted"), root.Join("installed"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("missing default config") != std::string::npos);
}

MT_TEST(extract_package_rejects_an_unsupported_format) {
    TempRoot root;
    root.WriteFile("package.zip", "PK");

    const agentenv::core::Expected<Unit, std::string> result =
        obd::ExtractPackage(root.Join("package.zip"), root.Join("out"));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("expected .tar.gz") != std::string::npos);
}

// ---------------------------------------------------------------------------
// System default config
// ---------------------------------------------------------------------------

MT_TEST(system_default_config_is_readable_by_the_runtime_group) {
    // Rust: `system_default_config_is_readable_by_the_runtime_group`.
    TempRoot root;
    root.WriteFile("staged/overlaybd.json", "{\"source\":true}\n");

    const std::string destination = root.Join("etc/overlaybd/overlaybd.json");
    const uint32_t runtime_gid = static_cast<uint32_t>(::getgid());

    MT_EXPECT_TRUE(
        obd::InstallDefaultConfig(root.Join("staged/overlaybd.json"), destination, runtime_gid)
            .ok());

    // The file must be group-readable and the directory group-traversable, so
    // the non-root runtime account can reach it.
    MT_EXPECT_EQ(fs::Stat(destination).value().mode, static_cast<uint32_t>(0640));
    MT_EXPECT_EQ(fs::Stat(destination).value().gid, runtime_gid);

    const Optional<std::string> parent = fs::Parent(destination);
    MT_EXPECT_TRUE(parent.has_value());
    MT_EXPECT_EQ(fs::Stat(*parent).value().mode, static_cast<uint32_t>(0750));
    MT_EXPECT_EQ(fs::Stat(*parent).value().gid, runtime_gid);
}

MT_TEST(system_default_config_retains_operator_edits) {
    // Rust's same test continues: an existing config's *content* survives a
    // re-run, but its mode is reasserted.
    TempRoot root;
    root.WriteFile("staged/overlaybd.json", "{\"source\":true}\n");
    const std::string destination = root.Join("etc/overlaybd/overlaybd.json");
    const uint32_t runtime_gid = static_cast<uint32_t>(::getgid());

    MT_EXPECT_TRUE(
        obd::InstallDefaultConfig(root.Join("staged/overlaybd.json"), destination, runtime_gid)
            .ok());

    // Operator tunes the config and tightens its mode.
    MT_EXPECT_TRUE(fs::Write(destination, "{\"custom\":true}\n").ok());
    MT_EXPECT_TRUE(fs::SetPermissions(destination, 0600).ok());

    MT_EXPECT_TRUE(
        obd::InstallDefaultConfig(root.Join("staged/overlaybd.json"), destination, runtime_gid)
            .ok());

    // Content kept...
    MT_EXPECT_EQ(fs::ReadToString(destination).value(), std::string("{\"custom\":true}\n"));
    // ...but the group-readable mode is restored, since the runtime must read it.
    MT_EXPECT_EQ(fs::Stat(destination).value().mode, static_cast<uint32_t>(0640));
}

MT_TEST(system_default_config_rejects_a_non_regular_destination) {
    TempRoot root;
    root.WriteFile("staged/overlaybd.json", "{}\n");
    // A directory where the config should be.
    MT_EXPECT_TRUE(fs::CreateDirAll(root.Join("etc/overlaybd/overlaybd.json")).ok());

    const agentenv::core::Expected<Unit, std::string> result = obd::InstallDefaultConfig(
        root.Join("staged/overlaybd.json"), root.Join("etc/overlaybd/overlaybd.json"),
        static_cast<uint32_t>(::getgid()));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("not a regular file") != std::string::npos);
}

MT_TEST(system_default_config_reports_a_missing_source) {
    TempRoot root;
    const agentenv::core::Expected<Unit, std::string> result = obd::InstallDefaultConfig(
        root.Join("staged/never-written.json"), root.Join("etc/overlaybd/overlaybd.json"),
        static_cast<uint32_t>(::getgid()));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("staged overlaybd default config is missing") !=
                   std::string::npos);
}

// ---------------------------------------------------------------------------
// EnsureReleaseTools / converter pinning
// ---------------------------------------------------------------------------

MT_TEST(ensure_release_tools_skips_a_current_install) {
    TempRoot root;
    const std::string install = root.Join("overlaybd");

    const obd::DependencyConfig config =
        ConfigWith("v1", "https://127.0.0.1:1/overlaybd-tools-{version}-linux-{arch}.tar.gz");
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> configured =
        obd::ConfiguredReleaseFor(config, "x86_64");
    MT_EXPECT_TRUE(configured.ok());

    // Pre-create a complete install matching the marker.
    WriteCompleteReleasePayload(root, "overlaybd");
    const obd::InstalledRelease desired = obd::DesiredInstalledRelease(
        configured.value(), "overlaybd-tools-v1-linux-x86_64.tar.gz");
    root.WriteFile("overlaybd/tools-release.json", desired.ToJson());

    // The URL is unreachable, so success proves nothing was downloaded.
    MT_EXPECT_TRUE(obd::EnsureReleaseTools(config, install, "x86_64").ok());
}

MT_TEST(ensure_release_tools_reinstalls_when_the_marker_mismatches) {
    TempRoot root;
    WriteCompleteReleasePayload(root, "overlaybd");
    // Marker names a different release, so the install is not current.
    root.WriteFile("overlaybd/tools-release.json",
                   "{\"tag_name\":\"other\",\"asset_name\":\"other.tar.gz\",\"digest\":null}");

    const obd::DependencyConfig config =
        ConfigWith("v1", "http://127.0.0.1:1/overlaybd-tools-{version}-linux-{arch}.tar.gz");
    // It must attempt the download (and fail), rather than skip.
    const agentenv::core::Expected<Unit, std::string> result =
        obd::EnsureReleaseTools(config, root.Join("overlaybd"), "x86_64");
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("GET http://127.0.0.1:1/") != std::string::npos);
}

MT_TEST(ensure_release_tools_reinstalls_when_a_tool_is_missing) {
    TempRoot root;
    const obd::DependencyConfig config =
        ConfigWith("v1", "http://127.0.0.1:1/overlaybd-tools-{version}-linux-{arch}.tar.gz");
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> configured =
        obd::ConfiguredReleaseFor(config, "x86_64");
    MT_EXPECT_TRUE(configured.ok());

    // Marker matches and the config is staged, but a tool is absent: the
    // previous install was incomplete and must be redone.
    WriteCompleteReleasePayload(root, "overlaybd");
    MT_EXPECT_TRUE(fs::RemoveFile(root.Join("overlaybd/bin/overlaybd-commit")).ok());
    root.WriteFile("overlaybd/tools-release.json",
                   obd::DesiredInstalledRelease(configured.value(),
                                                "overlaybd-tools-v1-linux-x86_64.tar.gz")
                       .ToJson());

    MT_EXPECT_TRUE(!obd::EnsureReleaseTools(config, root.Join("overlaybd"), "x86_64").ok());
}

MT_TEST(ensure_release_tools_reinstalls_when_the_staged_config_is_missing) {
    TempRoot root;
    const obd::DependencyConfig config =
        ConfigWith("v1", "http://127.0.0.1:1/overlaybd-tools-{version}-linux-{arch}.tar.gz");
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> configured =
        obd::ConfiguredReleaseFor(config, "x86_64");

    WriteCompleteReleasePayload(root, "overlaybd");
    MT_EXPECT_TRUE(fs::RemoveFile(root.Join("overlaybd/etc/overlaybd/overlaybd.json")).ok());
    root.WriteFile("overlaybd/tools-release.json",
                   obd::DesiredInstalledRelease(configured.value(),
                                                "overlaybd-tools-v1-linux-x86_64.tar.gz")
                       .ToJson());

    // All three conditions are checked, not just the marker.
    MT_EXPECT_TRUE(!obd::EnsureReleaseTools(config, root.Join("overlaybd"), "x86_64").ok());
}

MT_TEST(ensure_tools_converter_v1_reuses_a_matching_runtime_install) {
    // Rust: the second half of
    // `tools_converter_is_installed_once_and_retained_across_upgrades`.
    TempRoot root;
    const agentenv::core::Expected<std::string, std::string> arch = deps::DetectArch();
    if (!arch.ok()) return;  // unsupported host architecture

    const obd::DependencyConfig config =
        ConfigWith(obd::kConverterV1Version, obd::kConverterV1PackageUrl);
    const agentenv::core::Expected<obd::ConfiguredRelease, std::string> configured =
        obd::ConfiguredReleaseFor(config, arch.value());
    MT_EXPECT_TRUE(configured.ok());

    std::string asset = "overlaybd-tools-";
    asset += obd::kConverterV1Version;
    asset += "-linux-" + arch.value() + ".tar.gz";

    // The runtime install *is* converter v1, so it should be reused.
    WriteCompleteReleasePayload(root, "overlaybd");
    root.WriteFile("overlaybd/tools-release.json",
                   obd::DesiredInstalledRelease(configured.value(), asset).ToJson());

    const agentenv::core::Expected<std::string, std::string> resolved =
        obd::EnsureToolsConverterV1(root.path);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value(), root.Join("overlaybd"));
}

MT_TEST(ensure_tools_converter_v1_pins_when_the_runtime_moved_on) {
    // The runtime has been upgraded past v1, so the converter must not reuse
    // it — OCI tools v1 has to keep the same block layout.
    TempRoot root;
    if (!deps::DetectArch().ok()) return;

    WriteCompleteReleasePayload(root, "overlaybd");
    root.WriteFile("overlaybd/tools-release.json",
                   "{\"tag_name\":\"future-runtime-release\",\"asset_name\":\"other.tar.gz\","
                   "\"digest\":null}");

    // It now tries to provision the pinned copy, which fails without network
    // access — but the important part is that it did *not* return `overlaybd`.
    const agentenv::core::Expected<std::string, std::string> resolved =
        obd::EnsureToolsConverterV1(root.path);
    if (resolved.ok()) {
        MT_EXPECT_EQ(resolved.value(), root.Join("tools-converter-v1"));
    } else {
        // The failure must come from the download, not from the decision.
        MT_EXPECT_TRUE(resolved.error().find("GET ") != std::string::npos);
    }
}

int main() { return microtest::RunAll(); }
