// SPDX-License-Identifier: MIT
// Rust: src/deps/mod.rs :: prepare_tools_image (+ src/sandbox/tools_image.rs,
// which is just the binder around it).
//
// Upstream has no inline tests for this function, so what is pinned here is
// the set of judgements its comments claim: the probe order, which outcomes
// are "legacy ext4" rather than failures, that an absent resolver fails loudly
// instead of silently choosing a shape, and that the install is skipped when
// an image config is already in place.
//
// Everything runs against the real `PrepareToolsImage` with a scripted
// resolver; only the registry round trip is stood in for, because that is the
// injected seam upstream itself provides.
#include "agentenv/setup/deps.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/setup.h"
#include "microtest.h"

namespace deps = agentenv::setup::deps;
namespace core = agentenv::core;

namespace {

/// A unique scratch deps root per test, so nothing observes another's files.
std::string MakeDepsRoot(const std::string& tag) {
    const std::string root = "/tmp/agentenv-tools-" + tag + "-" + std::to_string(::getpid());
    core::fs::RemoveDirAll(root);
    if (!core::fs::CreateDirAll(root).ok()) return std::string();
    return root;
}

deps::Manifest TestManifest() {
    deps::Manifest manifest;
    manifest.tools_url = "ghcr.io/agentenv/tools:{version}";
    return manifest;
}

agentenv::cfg::AppConfig TestConfig(const std::string& deps_root) {
    agentenv::cfg::AppConfig config;
    config.deps_path = deps_root;
    config.tools.version = core::Optional<std::string>(std::string("0.1.0"));
    return config;
}

/// Records every reference it is asked about and replays one scripted answer.
struct ScriptedResolver {
    std::vector<std::string>* seen;
    /// The image config path to report, or empty for "no overlaybd image".
    std::string reply;
    bool fail;

    core::Expected<core::Optional<std::string>, std::string> operator()(
        const std::string& image_ref) const {
        if (seen != NULL) seen->push_back(image_ref);
        if (fail) return core::make_unexpected(std::string("registry unreachable"));
        if (reply.empty()) return core::Optional<std::string>();
        return core::Optional<std::string>(reply);
    }
};

deps::ToolsImageResolver Resolver(std::vector<std::string>* seen, const std::string& reply) {
    ScriptedResolver r;
    r.seen  = seen;
    r.reply = reply;
    r.fail  = false;
    return r;
}

deps::ToolsImageResolver FailingResolver(std::vector<std::string>* seen) {
    ScriptedResolver r;
    r.seen  = seen;
    r.reply = "";
    r.fail  = true;
    return r;
}

bool WriteFile(const std::string& path, const std::string& body) {
    const core::Optional<std::string> parent = core::fs::Parent(path);
    if (parent.has_value() && !core::fs::CreateDirAll(*parent).ok()) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == NULL) return false;
    if (!body.empty()) std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return true;
}

/// An overlaybd image config whose lowers are all *external* (no `file`), so
/// `InstallToolsImage` has nothing to hard-link and these tests stay about the
/// arbitration rather than about layer installation.
const char* const kImageConfigJson =
    "{\"repoBlobUrl\":\"\",\"lowers\":["
    "{\"digest\":\"sha256:base\",\"size\":4096,"
    "\"repoBlobUrl\":\"https://registry.example/v2/ns/tools/blobs\"}],"
    "\"upper\":{},\"resultFile\":\"\"}";

std::string ToolsPath(const std::string& root, const std::string& version) {
    return root + "/tools/" + version + "/tools.ext4";
}

std::string ImagePath(const std::string& root, const std::string& version) {
    return root + "/tools/" + version + "/image.json";
}

}  // namespace

// ---------------------------------------------------------------------------
// an installed legacy drive wins outright
// ---------------------------------------------------------------------------

MT_TEST(an_installed_ext4_drive_short_circuits_before_any_resolution) {
    // Checking the image config first would make a node that was provisioned
    // the legacy way start pulling from a registry on its next launch.
    const std::string root = MakeDepsRoot("ext4-wins");
    if (root.empty()) return;

    MT_EXPECT_TRUE(WriteFile(ToolsPath(root, "0.1.0"), "ext4 bytes"));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                Resolver(&seen, "/does/not/matter"));
    MT_EXPECT_TRUE(prepared.ok());
    // Empty optional == "attach the legacy ext4 file", not an error.
    MT_EXPECT_TRUE(!prepared.value().has_value());
    MT_EXPECT_TRUE(seen.empty());
    core::fs::RemoveDirAll(root);
}

MT_TEST(a_zero_length_ext4_drive_does_not_count_as_installed) {
    // A truncated download must not be mistaken for a provisioned drive.
    const std::string root = MakeDepsRoot("ext4-empty");
    if (root.empty()) return;

    MT_EXPECT_TRUE(WriteFile(ToolsPath(root, "0.1.0"), ""));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                Resolver(&seen, ""));
    // Falls through to resolution, which reports no overlaybd image, which
    // then takes the legacy download path. That path needs regctl, so the
    // outcome here is a failure -- what matters is that the empty file did
    // not short-circuit it.
    MT_EXPECT_TRUE(seen.size() == 1);
    core::fs::RemoveDirAll(root);
}

// ---------------------------------------------------------------------------
// an already-installed image config skips the registry
// ---------------------------------------------------------------------------

MT_TEST(an_installed_image_config_is_returned_without_resolving) {
    // The resolve is a registry round trip; repeating it on every launch of an
    // already-installed release would make start latency depend on a network.
    const std::string root = MakeDepsRoot("image-cached");
    if (root.empty()) return;

    MT_EXPECT_TRUE(WriteFile(ImagePath(root, "0.1.0"), kImageConfigJson));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                Resolver(&seen, "/unused"));
    MT_EXPECT_TRUE(prepared.ok());
    MT_EXPECT_TRUE(prepared.value().has_value());
    if (prepared.value().has_value()) {
        MT_EXPECT_EQ(*prepared.value(), ImagePath(root, "0.1.0"));
    }
    MT_EXPECT_TRUE(seen.empty());
    core::fs::RemoveDirAll(root);
}

MT_TEST(a_zero_length_image_config_is_reinstalled) {
    const std::string root = MakeDepsRoot("image-empty");
    if (root.empty()) return;

    MT_EXPECT_TRUE(WriteFile(ImagePath(root, "0.1.0"), ""));
    const std::string source = root + "/source-image.json";
    MT_EXPECT_TRUE(WriteFile(source, kImageConfigJson));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                Resolver(&seen, source));
    MT_EXPECT_TRUE(prepared.ok());
    MT_EXPECT_TRUE(seen.size() == 1);
    MT_EXPECT_TRUE(core::fs::Exists(ImagePath(root, "0.1.0")));
    core::fs::RemoveDirAll(root);
}

// ---------------------------------------------------------------------------
// resolution and install
// ---------------------------------------------------------------------------

MT_TEST(a_resolved_image_is_installed_beside_the_tools_drive) {
    const std::string root = MakeDepsRoot("install");
    if (root.empty()) return;

    const std::string source = root + "/source-image.json";
    MT_EXPECT_TRUE(WriteFile(source, kImageConfigJson));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                Resolver(&seen, source));
    MT_EXPECT_TRUE(prepared.ok());
    MT_EXPECT_TRUE(prepared.value().has_value());
    if (prepared.value().has_value()) {
        // Installed next to where the ext4 drive would have gone, so the whole
        // release lives in one directory and stays relocatable.
        MT_EXPECT_EQ(*prepared.value(), ImagePath(root, "0.1.0"));
        MT_EXPECT_TRUE(core::fs::Exists(*prepared.value()));
    }
    core::fs::RemoveDirAll(root);
}

MT_TEST(the_image_reference_is_built_from_the_requested_version) {
    // Not from the node's configured version: a resume must get the tools
    // release its snapshot was captured against.
    const std::string root = MakeDepsRoot("ref-version");
    if (root.empty()) return;

    const std::string source = root + "/source-image.json";
    MT_EXPECT_TRUE(WriteFile(source, kImageConfigJson));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.2.5",
                                Resolver(&seen, source));
    MT_EXPECT_TRUE(prepared.ok());
    MT_EXPECT_TRUE(seen.size() == 1);
    if (seen.size() == 1) {
        MT_EXPECT_EQ(seen[0], std::string("ghcr.io/agentenv/tools:0.2.5"));
    }
    if (prepared.value().has_value()) {
        MT_EXPECT_EQ(*prepared.value(), ImagePath(root, "0.2.5"));
    }
    core::fs::RemoveDirAll(root);
}

MT_TEST(a_configured_tools_url_overrides_the_manifest_template) {
    const std::string root = MakeDepsRoot("ref-override");
    if (root.empty()) return;

    const std::string source = root + "/source-image.json";
    MT_EXPECT_TRUE(WriteFile(source, kImageConfigJson));
    agentenv::cfg::AppConfig config = TestConfig(root);
    config.tools.url = core::Optional<std::string>(std::string("registry.local/t:{version}"));
    std::vector<std::string> seen;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(config, TestManifest(), "0.1.0", Resolver(&seen, source));
    MT_EXPECT_TRUE(prepared.ok());
    MT_EXPECT_TRUE(seen.size() == 1);
    if (seen.size() == 1) {
        MT_EXPECT_EQ(seen[0], std::string("registry.local/t:0.1.0"));
    }
    core::fs::RemoveDirAll(root);
}

MT_TEST(a_resolve_failure_is_propagated_rather_than_falling_back) {
    // A registry that cannot be reached is not the same as a release that
    // ships no overlaybd image. Treating it as the latter would silently
    // attach a different drive shape than the node is configured for.
    const std::string root = MakeDepsRoot("resolve-fail");
    if (root.empty()) return;

    std::vector<std::string> seen;
    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                FailingResolver(&seen));
    MT_EXPECT_TRUE(!prepared.ok());
    MT_EXPECT_TRUE(prepared.error().find("registry unreachable") != std::string::npos);
    MT_EXPECT_TRUE(!core::fs::Exists(ImagePath(root, "0.1.0")));
    core::fs::RemoveDirAll(root);
}

MT_TEST(an_absent_resolver_fails_instead_of_picking_a_shape) {
    // The default (no binder installed) must not be read as "this release has
    // no overlaybd image": that would quietly take the legacy path on a node
    // whose operator installed neither, and the error names what to do.
    const std::string root = MakeDepsRoot("no-resolver");
    if (root.empty()) return;

    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0",
                                deps::ToolsImageResolver());
    MT_EXPECT_TRUE(!prepared.ok());
    MT_EXPECT_TRUE(prepared.error().find("no tools image resolver") != std::string::npos);
    MT_EXPECT_TRUE(prepared.error().find("0.1.0") != std::string::npos);
    core::fs::RemoveDirAll(root);
}

// ---------------------------------------------------------------------------
// version validation comes from the path resolver
// ---------------------------------------------------------------------------

MT_TEST(an_invalid_version_is_refused_before_anything_is_resolved) {
    // The version lands in a filesystem path and in an image reference, so a
    // value that is not a plain SemVer is refused up front rather than
    // escaping into either.
    const std::string root = MakeDepsRoot("bad-version");
    if (root.empty()) return;

    std::vector<std::string> seen;
    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "../../etc",
                                Resolver(&seen, "/unused"));
    MT_EXPECT_TRUE(!prepared.ok());
    MT_EXPECT_TRUE(seen.empty());
    core::fs::RemoveDirAll(root);
}

MT_TEST(a_version_with_build_metadata_is_refused) {
    const std::string root = MakeDepsRoot("build-meta");
    if (root.empty()) return;

    std::vector<std::string> seen;
    const core::Expected<core::Optional<std::string>, std::string> prepared =
        deps::PrepareToolsImage(TestConfig(root), TestManifest(), "0.1.0+build7",
                                Resolver(&seen, "/unused"));
    MT_EXPECT_TRUE(!prepared.ok());
    MT_EXPECT_TRUE(seen.empty());
    core::fs::RemoveDirAll(root);
}

// ---------------------------------------------------------------------------
// manifest discovery
// ---------------------------------------------------------------------------

MT_TEST(the_deps_manifest_override_is_returned_verbatim) {
    // Returned even when absent, so a typo in the override is reported as a
    // missing manifest at *that* path rather than silently resolving to a
    // different file than the operator chose.
    ::setenv("AGENTENV_DEPS_MANIFEST", "/nonexistent/picked-by-operator.toml", 1);
    const core::Optional<std::string> found = agentenv::setup::FindDepsManifest();
    ::unsetenv("AGENTENV_DEPS_MANIFEST");

    MT_EXPECT_TRUE(found.has_value());
    if (found.has_value()) {
        MT_EXPECT_EQ(*found, std::string("/nonexistent/picked-by-operator.toml"));
    }
}

MT_TEST(an_empty_deps_manifest_override_is_ignored) {
    // An exported-but-empty variable is how a shell script passes "unset";
    // treating it as a path would break discovery entirely.
    ::setenv("AGENTENV_DEPS_MANIFEST", "", 1);
    const core::Optional<std::string> found = agentenv::setup::FindDepsManifest();
    ::unsetenv("AGENTENV_DEPS_MANIFEST");

    if (found.has_value()) {
        MT_EXPECT_TRUE(found->find("deps_manifest.toml") != std::string::npos);
    }
}

MT_MAIN
