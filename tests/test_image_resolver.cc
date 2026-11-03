// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — `mod tests`, the cases over the free
// functions. Cases that need a constructed `ImageResolver` (and therefore the
// image cache service) are not here.
#include "microtest.h"

#include <sys/stat.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/image/resolver.h"

using namespace agentenv;
using namespace agentenv::image;

namespace {

core::Optional<OverlaybdReferrer> Parse(const std::string& body) {
    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> parsed =
        ParseOverlaybdReferrer(body);
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

std::string Index(const std::string& manifests) {
    return "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.index.v1+json\","
           "\"manifests\":[" +
           manifests + "]}";
}

std::string Descriptor(const std::string& digest, const std::string& artifact_type) {
    return "{\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"digest\":\"" + digest +
           "\",\"size\":10,\"artifactType\":\"" + artifact_type + "\"}";
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct TempDir {
    std::string path;
    TempDir() { path = core::fs::CreateTempDir("agentenv-resolver-").value(); }
    ~TempDir() { core::fs::RemoveDirAll(path); }
};

/// Rust test helper `fake_regctl`: records argv one per line into
/// `<dir>/argv`, replays `stdout`/`stderr`, exits with `exit_code`.
std::string FakeRegctl(const std::string& dir, const std::string& stdout_text,
                       const std::string& stderr_text, int exit_code) {
    MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(dir, "stdout"), stdout_text).ok());
    MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(dir, "stderr"), stderr_text).ok());
    std::ostringstream script;
    script << "#!/bin/sh\ndir=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\nprintf '%s\\n' \"$@\" > "
              "\"$dir/argv\"\ncat \"$dir/stdout\"\ncat \"$dir/stderr\" >&2\nexit "
           << exit_code << "\n";
    const std::string binary = core::fs::Join(dir, "regctl");
    MT_EXPECT_TRUE(core::fs::Write(binary, script.str()).ok());
    MT_EXPECT_TRUE(chmod(binary.c_str(), 0755) == 0);
    return binary;
}

std::vector<std::string> RecordedArgv(const std::string& dir) {
    std::vector<std::string> out;
    std::istringstream lines(core::fs::ReadToString(core::fs::Join(dir, "argv")).value());
    for (std::string line; std::getline(lines, line);) out.push_back(line);
    return out;
}

}  // namespace

MT_TEST(registry_auth_validates_selected_image_format) {
    struct Case {
        ImageFormat format;
        bool has_auth;
        bool rejected;
    } const cases[] = {
        {ImageFormat::StandardOci, false, false},
        {ImageFormat::StandardOci, true, false},
        {ImageFormat::OverlaybdNative, false, false},
        {ImageFormat::OverlaybdNative, true, true},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const core::Expected<core::Unit, ImageError> result =
            ValidateRegistryAuthForFormat(cases[i].format, cases[i].has_auth);
        if (cases[i].rejected) {
            MT_EXPECT_TRUE(!result.ok());
            MT_EXPECT_TRUE(result.error().kind == ImageError::kUnsupportedImage);
            MT_EXPECT_TRUE(result.error().IsUserError());
        } else {
            MT_EXPECT_TRUE(result.ok());
        }
    }
}

MT_TEST(should_try_overlaybd_referrers_matches_domain_or_namespace_prefixes) {
    std::vector<std::string> prefixes;
    prefixes.push_back("registry.example.com/team/");
    prefixes.push_back("other.example.com/");
    MT_EXPECT_TRUE(ShouldTryOverlaybdReferrers("registry.example.com/team/app:tag", prefixes));
    MT_EXPECT_TRUE(
        ShouldTryOverlaybdReferrers("registry.example.com/team/app@sha256:abc", prefixes));
    MT_EXPECT_TRUE(ShouldTryOverlaybdReferrers("other.example.com/app:tag", prefixes));
    MT_EXPECT_TRUE(!ShouldTryOverlaybdReferrers("registry.example.com/team2/app:tag", prefixes));
}

MT_TEST(parse_overlaybd_referrer_picks_matching_artifact) {
    const core::Optional<OverlaybdReferrer> got =
        Parse(Index(Descriptor("sha256:other", "application/vnd.example.other") + "," +
                    Descriptor("sha256:overlaybd",
                               "application/vnd.containerd.overlaybd.native.v1+json")));
    MT_EXPECT_TRUE(got.has_value() && got->first == "sha256:overlaybd");
}

MT_TEST(parse_overlaybd_referrer_keeps_first_matching_artifact) {
    const core::Optional<OverlaybdReferrer> got = Parse(
        Index(Descriptor("sha256:first", "application/vnd.containerd.overlaybd.native.v1+json") +
              "," +
              Descriptor("sha256:second", "application/vnd.containerd.overlaybd.native.v1+json")));
    MT_EXPECT_TRUE(got.has_value() && got->first == "sha256:first");
}

MT_TEST(parse_overlaybd_referrer_returns_none_without_match) {
    MT_EXPECT_TRUE(!Parse(Index("")).has_value());
}

MT_TEST(parse_overlaybd_referrer_accepts_acr_artifact_streaming) {
    const core::Optional<OverlaybdReferrer> got =
        Parse(Index("{\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"digest\":"
                    "\"sha256:acr\",\"size\":1234,\"artifactType\":\"application/"
                    "vnd.azure.artifact.streaming.v1\",\"annotations\":{\"streaming.format\":"
                    "\"overlaybd\",\"streaming.version\":\"v1\",\"streaming.platform.os\":"
                    "\"linux\",\"streaming.platform.arch\":\"amd64\"}}"));
    MT_EXPECT_TRUE(got.has_value());
    MT_EXPECT_TRUE(got->first == "sha256:acr");
    MT_EXPECT_TRUE(got->second == "application/vnd.azure.artifact.streaming.v1");
}

MT_TEST(parse_overlaybd_referrer_selects_first_of_acr_per_platform_referrers) {
    const std::string amd =
        "sha256:1bdbcce5b02202d0c4c204da05e6ffbb8605bb178f441ff2990b9924c046a69f";
    const std::string arm =
        "sha256:3a5ae9547e3ed4d44f435db3ca286f9655e9fcb43ba36b0f5a6cf7da409ce0b8";
    const core::Optional<OverlaybdReferrer> got =
        Parse(Index(Descriptor(amd, "application/vnd.azure.artifact.streaming.v1") + "," +
                    Descriptor(arm, "application/vnd.azure.artifact.streaming.v1")));
    MT_EXPECT_TRUE(got.has_value());
    MT_EXPECT_TRUE(got->first == amd);
    MT_EXPECT_TRUE(got->second == "application/vnd.azure.artifact.streaming.v1");
}

MT_TEST(parse_overlaybd_referrer_prefers_containerd_native_over_acr_streaming) {
    const core::Optional<OverlaybdReferrer> got = Parse(
        Index(Descriptor("sha256:acr", "application/vnd.azure.artifact.streaming.v1") + "," +
              Descriptor("sha256:native", "application/vnd.containerd.overlaybd.native.v1+json")));
    MT_EXPECT_TRUE(got.has_value());
    MT_EXPECT_TRUE(got->first == "sha256:native");
    MT_EXPECT_TRUE(got->second == "application/vnd.containerd.overlaybd.native.v1+json");
}

MT_TEST(parse_overlaybd_referrer_ignores_unrelated_artifact_types) {
    MT_EXPECT_TRUE(!Parse(Index(Descriptor("sha256:sbom", "application/spdx+json") + "," +
                                Descriptor("sha256:turbo",
                                           "application/vnd.containerd.overlaybd.turbo.v1+json")))
                        .has_value());
}

MT_TEST(overlaybd_referrer_artifact_types_match_published_wire_values) {
    MT_EXPECT_TRUE(std::string(kOverlaybdNativeArtifactType) ==
                   "application/vnd.containerd.overlaybd.native.v1+json");
    MT_EXPECT_TRUE(std::string(kAcrArtifactStreamingArtifactType) ==
                   "application/vnd.azure.artifact.streaming.v1");
    const std::vector<std::string>& types = OverlaybdReferrerArtifactTypes();
    MT_EXPECT_TRUE(types.size() == 2u);
    MT_EXPECT_TRUE(types[0] == "application/vnd.containerd.overlaybd.native.v1+json");
    MT_EXPECT_TRUE(types[1] == "application/vnd.azure.artifact.streaming.v1");
}

// serde shape checks beyond the Rust cases: a missing `manifests` is empty, a
// descriptor without `digest` is an error, null `artifactType` is None.
MT_TEST(parse_overlaybd_referrer_follows_serde_defaults) {
    MT_EXPECT_TRUE(!Parse("{}").has_value());
    MT_EXPECT_TRUE(!ParseOverlaybdReferrer("{\"manifests\":null}").ok());
    MT_EXPECT_TRUE(!ParseOverlaybdReferrer("{\"manifests\":[{\"artifactType\":\"x\"}]}").ok());
    MT_EXPECT_TRUE(!Parse("{\"manifests\":[{\"digest\":\"sha256:a\",\"artifactType\":null}]}")
                        .has_value());
    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> bad =
        ParseOverlaybdReferrer("not json");
    MT_EXPECT_TRUE(!bad.ok());
    MT_EXPECT_TRUE(Contains(bad.error(), "parse referrers index JSON"));
}

MT_TEST(discover_overlaybd_referrer_lists_referrers_unfiltered_and_finds_acr_streaming) {
    TempDir temp;
    const std::string subject = "demo.azurecr.io/python:3.11-slim";
    const std::string digest =
        "sha256:0a21030948e9223e054ab830dd82b0ad85f921df34f11c5bf769bb0ed636d72a";
    const std::string regctl = FakeRegctl(
        temp.path, Index(Descriptor(digest, "application/vnd.azure.artifact.streaming.v1")), "",
        0);

    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> discovered =
        DiscoverOverlaybdReferrer(regctl, subject, core::Optional<std::string>());
    MT_EXPECT_TRUE(discovered.ok());
    MT_EXPECT_TRUE(discovered.value().has_value());
    MT_EXPECT_TRUE(discovered.value()->first == digest);
    MT_EXPECT_TRUE(discovered.value()->second == "application/vnd.azure.artifact.streaming.v1");

    const std::vector<std::string> argv = RecordedArgv(temp.path);
    const char* const expected[] = {"artifact", "list", "--format", "body",
                                    "demo.azurecr.io/python:3.11-slim"};
    MT_EXPECT_TRUE(argv.size() == 5u);
    for (std::size_t i = 0; i < argv.size() && i < 5; ++i) {
        MT_EXPECT_TRUE(argv[i] == expected[i]);
        MT_EXPECT_TRUE(argv[i] != "--filter-artifact-type");
    }
}

MT_TEST(discover_overlaybd_referrer_surfaces_regctl_failure) {
    TempDir temp;
    const std::string regctl =
        FakeRegctl(temp.path, "", "unauthorized: authentication required\n", 1);
    const core::Expected<core::Optional<OverlaybdReferrer>, std::string> error =
        DiscoverOverlaybdReferrer(regctl, "demo.azurecr.io/python:3.11-slim",
                                  core::Optional<std::string>());
    MT_EXPECT_TRUE(!error.ok());
    MT_EXPECT_TRUE(Contains(error.error(), "regctl artifact list failed"));
    MT_EXPECT_TRUE(Contains(error.error(), "unauthorized: authentication required"));
    // `str::trim`: the trailing newline is gone.
    MT_EXPECT_TRUE(error.error()[error.error().size() - 1] != '\n');
}

MT_TEST(image_resolve_error_classifies_user_vs_server_errors) {
    MT_EXPECT_TRUE(NotFound("missing").IsUserError());
    MT_EXPECT_TRUE(!OtherError("network down").IsUserError());
}

MT_TEST(overlaybd_image_config_json_preserves_local_layer_descriptor) {
    TempDir temp;
    const std::string layer_path = core::fs::Join(temp.path, "layer.commit");
    ResolvedImage resolved;
    resolved.kind = ResolvedImage::kLocal;
    LocalLayer layer;
    layer.path = layer_path;
    layer.digest = "sha256:commit";
    layer.size = 123;
    resolved.local_layers.push_back(layer);

    core::Json config = OverlaybdImageConfigJson(resolved);
    MT_EXPECT_TRUE(config["lowers"].as_array()[0]["file"].as_string() == layer_path);
    MT_EXPECT_TRUE(config["lowers"].as_array()[0]["digest"].as_string() == "sha256:commit");
    MT_EXPECT_TRUE(config["lowers"].as_array()[0]["size"].as_int() == 123);
}

MT_TEST(overlaybd_image_config_json_emits_remote_layer_dir) {
    TempDir temp;
    const std::string layer_dir = core::fs::Join(temp.path, "commits/sha256-abc");
    ResolvedImage resolved;
    resolved.kind = ResolvedImage::kRemote;
    resolved.repo_blob_url = "https://registry.example/v2/repo/blobs";
    RemoteLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 123;
    layer.dir = layer_dir;
    resolved.remote_layers.push_back(layer);

    core::Json config = OverlaybdImageConfigJson(resolved);
    MT_EXPECT_TRUE(config["lowers"].as_array()[0]["dir"].as_string() == layer_dir);
    MT_EXPECT_TRUE(config["lowers"].as_array()[0].as_object().count("cacheFile") == 0u);
    // serde_json without `preserve_order`: keys sorted.
    MT_EXPECT_TRUE(config.ToString() ==
                   "{\"lowers\":[{\"digest\":\"sha256:abc\",\"dir\":\"" + layer_dir +
                       "\",\"size\":123}],\"repoBlobUrl\":\"https://registry.example/v2/repo/"
                       "blobs\",\"resultFile\":\"\",\"upper\":{}}");
}

MT_TEST(registry_label_falls_back_to_unknown) {
    MT_EXPECT_TRUE(RegistryLabel("registry.example.com/team/app:tag") == "registry.example.com");
    MT_EXPECT_TRUE(RegistryLabel("ubuntu") == "unknown");
}

MT_MAIN
