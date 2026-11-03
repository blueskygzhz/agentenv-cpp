// SPDX-License-Identifier: MIT
// Rust: src/image/resolver.rs — `mod tests`, plus the
// `test_support::{write_built_image, store_layers}` helpers.
#include "microtest.h"

#include <sys/stat.h>

#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/image/catalog.h"
#include "agentenv/image/resolver.h"
#include "agentenv/snapshot/image_tag.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"
#include "agentenv/storage/overlaybd/config.h"

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

// ---- cases over a constructed resolver -------------------------------------

namespace {

/// Rust `test_resolver_with_search`.
std::unique_ptr<DefaultImageResolver> TestResolverWithSearch(
    const TempDir& temp, const std::vector<std::string>& search_registries) {
    cfg::AppConfig config;
    config.image.resolver.search_registries = search_registries;
    cfg::ImageConfig::Normalize(&config.image, temp.path, temp.path);
    return std::unique_ptr<DefaultImageResolver>(new DefaultImageResolver(config));
}

/// Rust `local_resolver`.
std::unique_ptr<DefaultImageResolver> LocalResolver(const TempDir& temp) {
    cfg::AppConfig config;
    cfg::ImageConfig::Normalize(&config.image, temp.path, temp.path);
    return std::unique_ptr<DefaultImageResolver>(new DefaultImageResolver(config));
}

/// Rust `PosixFsRepository::new(root)` — catalog and blobs under one root.
std::shared_ptr<snapshot::repository::SnapshotRepository> PosixRepository(
    const std::string& root) {
    namespace posixfs = snapshot::repository::posixfs;
    return std::make_shared<posixfs::PosixFsSnapshotRepository>(
        std::make_shared<posixfs::PosixFsCatalogStore>(root),
        std::make_shared<posixfs::PosixFsArtifactStore>(root));
}

/// Rust `test_support::write_built_image`.
std::string WriteBuiltImage(const std::string& root) {
    MT_EXPECT_TRUE(core::fs::CreateDirAll(root).ok());
    const std::string layer = core::fs::Join(root, "base.overlaybd.commit");
    MT_EXPECT_TRUE(core::fs::Write(layer, "base-layer").ok());
    const core::Expected<core::FileDigest, std::string> descriptor = core::DescribeFile(layer);
    MT_EXPECT_TRUE(descriptor.ok());
    core::Json lower = core::Json(core::JsonObject());
    lower["file"] = layer;
    lower["digest"] = descriptor.value().sha256;
    lower["size"] = static_cast<int64_t>(descriptor.value().size);
    core::Json config = core::Json(core::JsonObject());
    config["repoBlobUrl"] = "";
    config["lowers"] = core::Json(core::JsonArray(1, lower));
    config["upper"] = core::Json(core::JsonObject());
    config["resultFile"] = "";
    const std::string path = core::fs::Join(root, "image.json");
    MT_EXPECT_TRUE(core::fs::Write(path, config.ToString()).ok());
    return path;
}

/// Rust `test_support::store_layers`.
snapshot::repository::RepositoryResult<std::vector<snapshot::OverlaybdLayerRef> > StoreLayers(
    snapshot::repository::SnapshotRepository& repository, const std::string& image_config_path) {
    namespace repo = snapshot::repository;
    const core::Expected<storage::overlaybd::ImageConfig, std::string> config =
        storage::overlaybd::LoadImageConfig(image_config_path);
    if (!config.ok()) {
        return core::make_unexpected(repo::RepositoryError::Backend("load image config",
                                                                     config.error()));
    }
    std::vector<snapshot::OverlaybdLayerRef> layers;
    for (std::size_t i = 0; i < config.value().lowers.size(); ++i) {
        const storage::overlaybd::LayerConfig& lower = config.value().lowers[i];
        const repo::RepositoryResult<core::Unit> put = repository.PutBlob(
            repo::BlobKey::Layer(lower.digest, repo::LayerRole::Rootfs),
            repo::BlobBody::File(lower.file));
        if (!put.ok()) return core::make_unexpected(put.error());
        snapshot::ManagedLayer managed;
        managed.digest = lower.digest;
        managed.size = lower.size;
        layers.push_back(snapshot::OverlaybdLayerRef::Managed(managed));
    }
    return layers;
}

core::Json CmdConfig(const std::string& cmd) {
    core::Json config = core::Json(core::JsonObject());
    config["Cmd"] = core::Json(core::JsonArray(1, core::Json(cmd)));
    return config;
}

ResolvedBlockImage Built(const std::string& image_ref, const std::string& config_path,
                         const core::Json& raw) {
    ResolvedBlockImage built;
    built.image_ref = image_ref;
    built.overlaybd_config_path = config_path;
    built.raw_config = raw;
    return built;
}

}  // namespace

MT_TEST(registry_auth_rejects_ambiguous_search_before_access) {
    TempDir temp;
    std::vector<std::string> search;
    search.push_back("one.example");
    search.push_back("two.example");
    std::unique_ptr<DefaultImageResolver> resolver = TestResolverWithSearch(temp, search);
    const RegistryAuth auth = RegistryAuth::General("reader", "secret");
    const core::Expected<ResolvedBlockImage, ImageError> result =
        resolver->ResolveWithRegistryAuth("team/base", &auth);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().IsUserError());
    MT_EXPECT_TRUE(Contains(result.error().ToString(), "unambiguous"));
}

MT_TEST(registry_auth_rejects_published_images_before_repository_access) {
    TempDir temp;
    // No repository is configured: rejection must precede tag/digest lookup.
    std::unique_ptr<DefaultImageResolver> resolver =
        TestResolverWithSearch(temp, std::vector<std::string>(1, "registry.example"));
    const RegistryAuth auth = RegistryAuth::General("reader", "secret");
    const std::string digest = core::Sha256Digest(std::string("published image"));
    const char* const references[] = {" aenv/web:v1 ", digest.c_str()};
    for (std::size_t i = 0; i < 2; ++i) {
        const core::Expected<ResolvedBlockImage, ImageError> result =
            resolver->ResolveWithRegistryAuth(references[i], &auth);
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().kind == ImageError::kInvalidReference);
        MT_EXPECT_TRUE(Contains(result.error().ToString(),
                                "fromImageRegistry cannot be used with AgentENV published images"));
    }
}

MT_TEST(image_config_normalize_resolver_fields) {
    TempDir temp;
    cfg::ImageConfig config;
    config.resolver.default_image = " ghcr.io/example/base:latest ";
    config.resolver.search_registries.clear();
    config.resolver.search_registries.push_back(" registry.internal:5000/team/ ");
    config.resolver.search_registries.push_back("ghcr.io");
    config.resolver.try_referrers_overlaybd_prefixes.push_back(" registry.example.com/team/ ");
    config.resolver.try_referrers_overlaybd_prefixes.push_back("");
    config.resolver.try_referrers_overlaybd_prefixes.push_back("registry.example.com/");
    cfg::ImageConfig::Normalize(&config, temp.path, temp.path);

    MT_EXPECT_EQ(config.resolver.default_image, std::string("ghcr.io/example/base:latest"));
    std::vector<std::string> search;
    search.push_back("registry.internal:5000/team");
    search.push_back("ghcr.io");
    MT_EXPECT_TRUE(config.resolver.search_registries == search);
    std::vector<std::string> prefixes;
    prefixes.push_back("registry.example.com/team/");
    prefixes.push_back("registry.example.com/");
    MT_EXPECT_TRUE(config.resolver.try_referrers_overlaybd_prefixes == prefixes);
}

MT_TEST(image_config_normalize_allowed_registries_distinguishes_unset_and_empty) {
    TempDir temp;

    // Unset key => None (no restriction).
    cfg::ImageConfig unset;
    cfg::ImageConfig::Normalize(&unset, temp.path, temp.path);
    MT_EXPECT_TRUE(!unset.resolver.allowed_registries.has_value());

    // Explicit empty list => Some([]) (deny all), distinct from None.
    cfg::ImageConfig empty;
    empty.resolver.allowed_registries = std::vector<std::string>();
    cfg::ImageConfig::Normalize(&empty, temp.path, temp.path);
    MT_EXPECT_TRUE(empty.resolver.allowed_registries.has_value());
    MT_EXPECT_TRUE(empty.resolver.allowed_registries->empty());

    // Non-empty list is trimmed, trailing slashes stripped, empties dropped.
    cfg::ImageConfig set;
    std::vector<std::string> allowed;
    allowed.push_back(" registry.example.com/ ");
    allowed.push_back("");
    allowed.push_back("   ");
    allowed.push_back("ghcr.io");
    set.resolver.allowed_registries = allowed;
    cfg::ImageConfig::Normalize(&set, temp.path, temp.path);
    std::vector<std::string> expected;
    expected.push_back("registry.example.com");
    expected.push_back("ghcr.io");
    MT_EXPECT_TRUE(set.resolver.allowed_registries.has_value());
    MT_EXPECT_TRUE(*set.resolver.allowed_registries == expected);
}

MT_TEST(image_config_normalize_blank_default_image_to_schema_default) {
    TempDir temp;
    cfg::ImageConfig config;
    config.resolver.default_image = "   ";
    cfg::ImageConfig::Normalize(&config, temp.path, temp.path);
    MT_EXPECT_EQ(config.resolver.default_image, cfg::ImageResolverConfig().default_image);
}

MT_TEST(resolver_uses_normalized_config_fields) {
    TempDir temp;
    cfg::AppConfig config;
    config.image.resolver.default_image = " ghcr.io/example/base:latest ";
    cfg::ImageConfig::Normalize(&config.image, temp.path, temp.path);
    const DefaultImageResolver resolver(config);
    MT_EXPECT_EQ(resolver.DefaultImage(), std::string("ghcr.io/example/base:latest"));
}

MT_TEST(resolve_rejects_empty_configured_search_list_for_shortnames) {
    TempDir temp;
    std::unique_ptr<DefaultImageResolver> resolver =
        TestResolverWithSearch(temp, std::vector<std::string>());
    const core::Expected<ResolvedBlockImage, ImageError> result = resolver->Resolve("ubuntu");
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == ImageError::kInvalidReference);
    MT_EXPECT_TRUE(result.error().IsUserError());
    MT_EXPECT_TRUE(Contains(result.error().ToString(), "search registry list"));
}

MT_TEST(published_image_survives_source_removal_and_resolves_on_another_node) {
    TempDir shared;
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository =
        PosixRepository(core::fs::Join(shared.path, "repository"));
    std::unique_ptr<TempDir> source(new TempDir());
    std::unique_ptr<TempDir> node_a(new TempDir());
    TempDir node_b;
    std::unique_ptr<DefaultImageResolver> publisher = LocalResolver(*node_a);
    publisher->WithRepository(repository);
    const core::Json raw = core::Json::Parse(
                               "{\"Env\":[\"A=B\"],\"Entrypoint\":[\"/app\"],\"Cmd\":[\"serve\"],"
                               "\"Healthcheck\":{\"Test\":[\"CMD\",\"true\"]}}")
                               .value();
    const ResolvedBlockImage built =
        Built(core::Sha256Digest(std::string("original OCI manifest")),
              WriteBuiltImage(source->path), raw);
    const core::Expected<std::string, std::string> digest =
        publisher->PublishImage(built, StoreLayers);
    MT_EXPECT_TRUE(digest.ok());
    MT_EXPECT_TRUE(digest.value() != built.image_ref);
    const core::Expected<std::string, std::string> again =
        publisher->PublishImage(built, StoreLayers);
    MT_EXPECT_TRUE(again.ok());
    MT_EXPECT_EQ(again.value(), digest.value());
    ResolvedBlockImage changed = built;
    changed.raw_config = CmdConfig("different");
    const core::Expected<std::string, std::string> changed_digest =
        publisher->PublishImage(changed, StoreLayers);
    MT_EXPECT_TRUE(changed_digest.ok());
    MT_EXPECT_TRUE(changed_digest.value() != digest.value());
    publisher.reset();
    source.reset();
    node_a.reset();

    std::unique_ptr<DefaultImageResolver> reader = LocalResolver(node_b);
    reader->WithRepository(repository);
    const core::Expected<ResolvedBlockImage, ImageError> resolved = reader->Resolve(digest.value());
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().image_ref, digest.value());
    MT_EXPECT_TRUE(resolved.value().raw_config.has_value());
    MT_EXPECT_TRUE(*resolved.value().raw_config == raw);
    std::map<std::string, std::string>::const_iterator a =
        resolved.value().base_context.env_vars.find("A");
    MT_EXPECT_TRUE(a != resolved.value().base_context.env_vars.end() && a->second == "B");
    const core::Json config =
        core::Json::Parse(
            core::fs::ReadToString(resolved.value().overlaybd_config_path).value())
            .value();
    const core::JsonArray& lowers = config["lowers"].as_array();
    MT_EXPECT_TRUE(!lowers.empty());
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        MT_EXPECT_TRUE(core::fs::IsFile(lowers[i]["file"].as_string()));
    }
    MT_EXPECT_TRUE(core::fs::RemoveFile(resolved.value().overlaybd_config_path).ok());
    const core::Expected<ResolvedBlockImage, ImageError> again_resolved =
        reader->Resolve(digest.value());
    MT_EXPECT_TRUE(again_resolved.ok());
    MT_EXPECT_TRUE(core::fs::IsFile(again_resolved.value().overlaybd_config_path));

    const core::Expected<ResolvedBlockImage, ImageError> unknown =
        reader->Resolve(core::Sha256Digest(std::string("unknown")));
    MT_EXPECT_TRUE(!unknown.ok() && unknown.error().kind == ImageError::kNotFound);
    const core::Expected<ResolvedBlockImage, ImageError> invalid =
        reader->Resolve("sha256:invalid");
    MT_EXPECT_TRUE(!invalid.ok() && invalid.error().kind == ImageError::kInvalidReference);

    const std::string path =
        core::fs::Join(core::fs::Join(shared.path, "repository"),
                       snapshot::repository::DocumentKey::Image(digest.value()).value().Path());
    core::Json description = core::Json::Parse(core::fs::ReadToString(path).value()).value();
    description["config"]["Cmd"] = core::Json(core::JsonArray(1, core::Json("tampered")));
    MT_EXPECT_TRUE(core::fs::Write(path, description.ToString()).ok());
    const core::Expected<ResolvedBlockImage, ImageError> tampered =
        reader->Resolve(digest.value());
    MT_EXPECT_TRUE(!tampered.ok());
    MT_EXPECT_TRUE(Contains(tampered.error().ToString(), "digest mismatch"));
}

MT_TEST(image_tags_resolve_to_the_digest_they_point_to_now) {
    TempDir shared;
    TempDir node;
    std::shared_ptr<snapshot::repository::SnapshotRepository> repository =
        PosixRepository(core::fs::Join(shared.path, "repository"));
    std::unique_ptr<DefaultImageResolver> resolver = LocalResolver(node);
    resolver->WithRepository(repository);
    ResolvedBlockImage built =
        Built(core::Sha256Digest(std::string("source manifest")),
              WriteBuiltImage(core::fs::Join(shared.path, "build")), CmdConfig("v1"));
    const core::Expected<std::string, std::string> first =
        resolver->PublishImage(built, StoreLayers);
    MT_EXPECT_TRUE(first.ok());
    built.raw_config = CmdConfig("v2");
    const core::Expected<std::string, std::string> second =
        resolver->PublishImage(built, StoreLayers);
    MT_EXPECT_TRUE(second.ok());

    const snapshot::ImageTag tag = snapshot::ImageTag::Parse("web:v1").value();
    MT_EXPECT_TRUE(PutImageTag(*repository, tag, first.value()).ok());
    core::Expected<ResolvedBlockImage, ImageError> resolved = resolver->Resolve(" aenv/web:v1 ");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().image_ref, first.value());
    MT_EXPECT_TRUE(*resolved.value().raw_config == CmdConfig("v1"));

    // The node cache is keyed by digest, so a moved tag is never served stale.
    MT_EXPECT_TRUE(PutImageTag(*repository, tag, second.value()).ok());
    resolved = resolver->Resolve("aenv/web:v1");
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().image_ref, second.value());
    MT_EXPECT_TRUE(*resolved.value().raw_config == CmdConfig("v2"));

    resolved = resolver->Resolve("aenv/web");
    MT_EXPECT_TRUE(!resolved.ok() && resolved.error().kind == ImageError::kNotFound);
    const snapshot::ImageTag gone = snapshot::ImageTag::Parse("web:gone").value();
    MT_EXPECT_TRUE(
        PutImageTag(*repository, gone, core::Sha256Digest(std::string("deleted"))).ok());
    resolved = resolver->Resolve("aenv/web:gone");
    MT_EXPECT_TRUE(!resolved.ok() && resolved.error().kind == ImageError::kNotFound);
    MT_EXPECT_TRUE(Contains(resolved.error().ToString(), "image tag 'web:gone'"));
    const char* const invalid[] = {"aenv/Web:v1", "aenv/", "aenv/web:v1:extra",
                                   "aenv/team/web:v1"};
    for (std::size_t i = 0; i < 4; ++i) {
        resolved = resolver->Resolve(invalid[i]);
        MT_EXPECT_TRUE(!resolved.ok() && resolved.error().kind == ImageError::kInvalidReference);
    }
    // With a registry host, an `aenv` namespace still names an OCI image;
    // denying every registry proves the reference took the OCI path.
    resolver->SetAllowedRegistriesForTesting(
        core::Optional<std::vector<std::string> >(std::vector<std::string>()));
    resolved = resolver->Resolve("docker.io/aenv/web:v1");
    MT_EXPECT_TRUE(!resolved.ok() && resolved.error().kind == ImageError::kInvalidReference &&
                   Contains(resolved.error().reason, "allowed registr"));
    MT_EXPECT_TRUE(!PutImageTag(*repository, tag, "sha256:short").ok());

    const std::string path =
        core::fs::Join(core::fs::Join(shared.path, "repository"),
                       snapshot::repository::DocumentKey::ImageTag(tag).Path());
    MT_EXPECT_TRUE(core::fs::Write(path, "\"not-a-digest\"").ok());
    resolved = resolver->Resolve("aenv/web:v1");
    MT_EXPECT_TRUE(!resolved.ok() && resolved.error().kind == ImageError::kOther);
}

MT_MAIN
