// SPDX-License-Identifier: MIT
// Rust: src/setup/deps.rs `mod tests`, plus the credential resolution from
// crates/object-store-operator/src/auth.rs that its OSS config depends on.
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "agentenv/core/credentials.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/setup/deps.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
using agentenv::core::VirtualizationMode;
namespace creds = agentenv::core::credentials;
namespace deps = agentenv::setup::deps;
namespace fs = agentenv::core::fs;
namespace obd = agentenv::storage::overlaybd;

struct TempRoot {
    std::string path;

    TempRoot() {
        const agentenv::core::Expected<std::string, std::string> temp =
            fs::CreateTempDir("agentenv-deps-");
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

std::vector<std::pair<std::string, std::string> > Vars(const std::string& k1,
                                                       const std::string& v1,
                                                       const std::string& k2 = std::string(),
                                                       const std::string& v2 = std::string()) {
    std::vector<std::pair<std::string, std::string> > vars;
    vars.push_back(std::make_pair(k1, v1));
    if (!k2.empty()) vars.push_back(std::make_pair(k2, v2));
    return vars;
}

/// Rust test helper `sample_oss_config` — every field deliberately padded with
/// whitespace, to prove normalization happens.
agentenv::cfg::OssBackendConfig SampleOssConfig() {
    agentenv::cfg::OssBackendConfig oss;
    oss.endpoint = " https://oss-cn-hangzhou.aliyuncs.com ";
    oss.bucket = " demo-bucket ";
    oss.prefix = std::string(" snapshots ");
    oss.access_key_id = std::string(" ak ");
    oss.access_key_secret = std::string(" sk ");
    oss.security_token = std::string(" token ");
    oss.region = std::string(" cn-hangzhou ");
    oss.cache_max_size_gb = static_cast<uint64_t>(4);
    return oss;
}

std::string JsonField(const agentenv::core::Json& object, const char* key) {
    MT_EXPECT_TRUE(object.kind() == agentenv::core::Json::Kind::Object);
    const agentenv::core::JsonObject& members = object.as_object();
    const agentenv::core::JsonObject::const_iterator found = members.find(key);
    MT_EXPECT_TRUE(found != members.end());
    MT_EXPECT_TRUE(found->second.kind() == agentenv::core::Json::Kind::String);
    return found->second.as_string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Credential resolution
// ---------------------------------------------------------------------------

MT_TEST(normalized_credential_treats_blank_as_absent) {
    MT_EXPECT_TRUE(!creds::Normalized(Optional<std::string>()).has_value());
    MT_EXPECT_TRUE(!creds::Normalized(Optional<std::string>(std::string(""))).has_value());
    MT_EXPECT_TRUE(!creds::Normalized(Optional<std::string>(std::string("  \t\n"))).has_value());
    // A present value is trimmed, not just accepted.
    MT_EXPECT_EQ(*creds::Normalized(Optional<std::string>(std::string("  ak  "))),
                 std::string("ak"));
}

MT_TEST(credential_source_resolves_static_pair) {
    creds::CredentialFields fields;
    fields.access_key_id = std::string(" ak ");
    fields.secret_access_key = std::string(" sk ");
    fields.security_token = std::string(" token ");

    creds::CredentialSourceOptions options;
    options.scope = "backend.oss";
    options.required_access_key_id_label = "backend.oss.access_key_id";
    options.required_secret_access_key_label = "backend.oss.access_key_secret";

    const agentenv::core::Expected<creds::CredentialSource, std::string> source =
        creds::FromFields(fields, options);
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_EQ(source.value().kind, creds::CredentialSource::Kind::Static);
    MT_EXPECT_EQ(source.value().credential.access_key_id, std::string("ak"));
    MT_EXPECT_EQ(source.value().credential.secret_access_key, std::string("sk"));
    MT_EXPECT_EQ(*source.value().credential.security_token, std::string("token"));
}

MT_TEST(credential_process_is_exclusive) {
    creds::CredentialSourceOptions options;
    options.scope = "backend.oss";
    options.required_access_key_id_label = "backend.oss.access_key_id";
    options.required_secret_access_key_label = "backend.oss.access_key_secret";

    // Alone it is fine.
    creds::CredentialFields process_only;
    process_only.credential_process = std::string("echo creds");
    const agentenv::core::Expected<creds::CredentialSource, std::string> ok =
        creds::FromFields(process_only, options);
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_EQ(ok.value().kind, creds::CredentialSource::Kind::Process);
    MT_EXPECT_EQ(ok.value().command, std::string("echo creds"));

    // Combined with a static field it must be rejected, not silently
    // preferred — otherwise a misconfiguration would be invisible.
    creds::CredentialFields mixed = process_only;
    mixed.access_key_id = std::string("ak");
    const agentenv::core::Expected<creds::CredentialSource, std::string> conflict =
        creds::FromFields(mixed, options);
    MT_EXPECT_TRUE(!conflict.ok());
    MT_EXPECT_TRUE(conflict.error().find("cannot be combined with") != std::string::npos);
}

MT_TEST(credential_source_rejects_incomplete_pairs) {
    creds::CredentialSourceOptions options;
    options.scope = "backend.oss";
    options.allow_anonymous = false;
    options.required_access_key_id_label = "backend.oss.access_key_id";
    options.required_secret_access_key_label = "backend.oss.access_key_secret";

    // Secret without id: the *id* is what is reported missing.
    creds::CredentialFields secret_only;
    secret_only.secret_access_key = std::string("sk");
    const agentenv::core::Expected<creds::CredentialSource, std::string> no_id =
        creds::FromFields(secret_only, options);
    MT_EXPECT_TRUE(!no_id.ok());
    MT_EXPECT_TRUE(no_id.error().find("backend.oss.access_key_id is required") !=
                   std::string::npos);

    // Id without secret: the *secret* is reported.
    creds::CredentialFields id_only;
    id_only.access_key_id = std::string("ak");
    const agentenv::core::Expected<creds::CredentialSource, std::string> no_secret =
        creds::FromFields(id_only, options);
    MT_EXPECT_TRUE(!no_secret.ok());
    MT_EXPECT_TRUE(no_secret.error().find("backend.oss.access_key_secret is required") !=
                   std::string::npos);

    // A lone token cannot authenticate anything.
    creds::CredentialFields token_only;
    token_only.security_token = std::string("token");
    const agentenv::core::Expected<creds::CredentialSource, std::string> lone_token =
        creds::FromFields(token_only, options);
    MT_EXPECT_TRUE(!lone_token.ok());
    MT_EXPECT_TRUE(lone_token.error().find("security_token requires") != std::string::npos);
}

MT_TEST(credential_source_anonymous_only_when_allowed) {
    creds::CredentialFields empty;

    creds::CredentialSourceOptions strict;
    strict.scope = "backend.oss";
    strict.allow_anonymous = false;
    strict.required_access_key_id_label = "backend.oss.access_key_id";
    strict.required_secret_access_key_label = "backend.oss.access_key_secret";
    MT_EXPECT_TRUE(!creds::FromFields(empty, strict).ok());

    creds::CredentialSourceOptions permissive = strict;
    permissive.allow_anonymous = true;
    const agentenv::core::Expected<creds::CredentialSource, std::string> anonymous =
        creds::FromFields(empty, permissive);
    MT_EXPECT_TRUE(anonymous.ok());
    MT_EXPECT_EQ(anonymous.value().kind, creds::CredentialSource::Kind::Anonymous);

    // When anonymous is allowed, a half pair reports the pairing rule instead.
    creds::CredentialFields half;
    half.access_key_id = std::string("ak");
    const agentenv::core::Expected<creds::CredentialSource, std::string> pairing =
        creds::FromFields(half, permissive);
    MT_EXPECT_TRUE(!pairing.ok());
    MT_EXPECT_TRUE(pairing.error().find("must be set together") != std::string::npos);
}

// ---------------------------------------------------------------------------
// OSS runtime config
// ---------------------------------------------------------------------------

MT_TEST(overlaybd_runtime_oss_config_uses_normalized_semantics) {
    // Rust: `overlaybd_runtime_oss_config_uses_normalized_semantics`.
    const agentenv::core::Expected<agentenv::core::Json, std::string> config =
        deps::OverlaybdRuntimeOssConfig(SampleOssConfig());
    MT_EXPECT_TRUE(config.ok());

    MT_EXPECT_EQ(JsonField(config.value(), "accessKeyId"), std::string("ak"));
    MT_EXPECT_EQ(JsonField(config.value(), "secretAccessKey"), std::string("sk"));
    MT_EXPECT_EQ(JsonField(config.value(), "securityToken"), std::string("token"));
    MT_EXPECT_EQ(JsonField(config.value(), "credentialProcess"), std::string(""));
    MT_EXPECT_EQ(JsonField(config.value(), "defaultRegion"), std::string("cn-hangzhou"));
    MT_EXPECT_EQ(JsonField(config.value(), "defaultEndpoint"),
                 std::string("https://oss-cn-hangzhou.aliyuncs.com"));
    // Empty means the runtime auto-detects per endpoint.
    MT_EXPECT_EQ(JsonField(config.value(), "defaultAddressingStyle"), std::string(""));
}

MT_TEST(overlaybd_runtime_oss_config_propagates_addressing_style) {
    // Rust: `overlaybd_runtime_oss_config_propagates_addressing_style`.
    agentenv::cfg::OssBackendConfig oss = SampleOssConfig();
    oss.addressing_style = agentenv::cfg::OssAddressingStyle::Virtual;
    MT_EXPECT_EQ(JsonField(deps::OverlaybdRuntimeOssConfig(oss).value(),
                           "defaultAddressingStyle"),
                 std::string("virtual"));

    oss.addressing_style = agentenv::cfg::OssAddressingStyle::Path;
    MT_EXPECT_EQ(JsonField(deps::OverlaybdRuntimeOssConfig(oss).value(),
                           "defaultAddressingStyle"),
                 std::string("path"));
}

MT_TEST(overlaybd_runtime_oss_config_uses_credential_process_when_configured) {
    // Rust: `overlaybd_runtime_oss_config_uses_credential_process_when_configured`.
    agentenv::cfg::OssBackendConfig oss = SampleOssConfig();
    oss.access_key_id = Optional<std::string>();
    oss.access_key_secret = Optional<std::string>();
    oss.security_token = Optional<std::string>();
    oss.credential_process = std::string("echo creds");

    const agentenv::core::Expected<agentenv::core::Json, std::string> config =
        deps::OverlaybdRuntimeOssConfig(oss);
    MT_EXPECT_TRUE(config.ok());
    // The static fields must be written empty, so the runtime cannot mix the
    // two mechanisms.
    MT_EXPECT_EQ(JsonField(config.value(), "accessKeyId"), std::string(""));
    MT_EXPECT_EQ(JsonField(config.value(), "secretAccessKey"), std::string(""));
    MT_EXPECT_EQ(JsonField(config.value(), "securityToken"), std::string(""));
    MT_EXPECT_EQ(JsonField(config.value(), "credentialProcess"), std::string("echo creds"));
}

MT_TEST(overlaybd_runtime_oss_config_rejects_blank_static_credentials) {
    // Rust: `overlaybd_runtime_oss_config_rejects_blank_static_credentials`.
    agentenv::cfg::OssBackendConfig oss = SampleOssConfig();
    oss.access_key_id = std::string("   ");

    const agentenv::core::Expected<agentenv::core::Json, std::string> config =
        deps::OverlaybdRuntimeOssConfig(oss);
    MT_EXPECT_TRUE(!config.ok());
    MT_EXPECT_TRUE(config.error().find("backend.oss.access_key_id") != std::string::npos);
}

MT_TEST(overlaybd_runtime_oss_config_requires_a_region) {
    agentenv::cfg::OssBackendConfig oss = SampleOssConfig();
    oss.region = std::string("   ");

    const agentenv::core::Expected<agentenv::core::Json, std::string> config =
        deps::OverlaybdRuntimeOssConfig(oss);
    MT_EXPECT_TRUE(!config.ok());
    MT_EXPECT_TRUE(config.error().find("backend.oss.region must be set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Credential file detection
// ---------------------------------------------------------------------------

MT_TEST(overlaybd_credential_fields_distinguish_present_from_absent) {
    std::string path;
    agentenv::core::Json config;

    deps::OverlaybdCredentialFields(Optional<std::string>(std::string("/creds/config.json")),
                                    &path, &config);
    MT_EXPECT_EQ(path, std::string("/creds/config.json"));
    MT_EXPECT_EQ(JsonField(config, "mode"), std::string("file"));
    MT_EXPECT_EQ(JsonField(config, "path"), std::string("/creds/config.json"));

    deps::OverlaybdCredentialFields(Optional<std::string>(), &path, &config);
    MT_EXPECT_EQ(path, std::string(""));
    // The empty state is a specific shape, not all-zero: the runtime
    // distinguishes mode "" with timeout 1 from mode "file".
    MT_EXPECT_EQ(JsonField(config, "mode"), std::string(""));
    MT_EXPECT_EQ(JsonField(config, "path"), std::string(""));
}

MT_TEST(docker_credential_config_prefers_docker_config_env) {
    TempRoot root;
    root.WriteFile("dockercfg/config.json", "{}");
    root.WriteFile("home/.docker/config.json", "{}");

    MT_EXPECT_EQ(::setenv("DOCKER_CONFIG", root.Join("dockercfg").c_str(), 1), 0);
    MT_EXPECT_EQ(::setenv("HOME", root.Join("home").c_str(), 1), 0);
    const Optional<std::string> preferred = deps::DetectDockerCredentialConfig();
    MT_EXPECT_TRUE(preferred.has_value());
    MT_EXPECT_EQ(*preferred, root.Join("dockercfg/config.json"));

    // Falls back to $HOME when DOCKER_CONFIG names nothing usable.
    MT_EXPECT_EQ(::setenv("DOCKER_CONFIG", root.Join("missing").c_str(), 1), 0);
    const Optional<std::string> fallback = deps::DetectDockerCredentialConfig();
    MT_EXPECT_TRUE(fallback.has_value());
    MT_EXPECT_EQ(*fallback, root.Join("home/.docker/config.json"));

    // Neither present: anonymous registry access, which is fine publicly.
    MT_EXPECT_EQ(::setenv("HOME", root.Join("nowhere").c_str(), 1), 0);
    MT_EXPECT_TRUE(!deps::DetectDockerCredentialConfig().has_value());
    ::unsetenv("DOCKER_CONFIG");
}

// ---------------------------------------------------------------------------
// URL templates and manifest
// ---------------------------------------------------------------------------

MT_TEST(resolve_url_substitutes_every_occurrence) {
    // The firecracker template uses `{version}` twice; replacing only the
    // first would produce a 404.
    MT_EXPECT_EQ(
        deps::ResolveUrl("https://x/static-{version}/tools-{version}-linux-{arch}.tar.gz",
                         Vars("version", "v1.2.3", "arch", "aarch64")),
        std::string("https://x/static-v1.2.3/tools-v1.2.3-linux-aarch64.tar.gz"));

    // An unknown placeholder is left alone rather than blanked.
    MT_EXPECT_EQ(deps::ResolveUrl("https://x/{unknown}", Vars("version", "v1")),
                 std::string("https://x/{unknown}"));
    MT_EXPECT_EQ(deps::ResolveUrl("https://x/plain", Vars("version", "v1")),
                 std::string("https://x/plain"));
}

MT_TEST(manifest_parses_the_bundled_file) {
    const agentenv::core::Expected<deps::Manifest, std::string> manifest =
        deps::Manifest::ParseFile("config/deps_manifest.toml");
    if (!manifest.ok()) return;  // not run from the source root

    MT_EXPECT_TRUE(!manifest.value().firecracker.kvm.version.empty());
    MT_EXPECT_TRUE(!manifest.value().firecracker.pvm.version.empty());
    MT_EXPECT_TRUE(!manifest.value().kernel.kvm.url.empty());
    MT_EXPECT_TRUE(!manifest.value().tools_url.empty());
    MT_EXPECT_TRUE(!manifest.value().overlaybd.version.empty());
    // The manifest spells this section `regclient`, not `regctl`.
    MT_EXPECT_TRUE(!manifest.value().regctl.version.empty());
    MT_EXPECT_TRUE(manifest.value().regctl.url.find("{arch}") != std::string::npos);
}

MT_TEST(manifest_selects_downloads_per_virtualization_mode) {
    const char* const toml =
        "[firecracker.kvm]\nversion = \"fc-kvm\"\nurl = \"https://x/kvm\"\n"
        "[firecracker.pvm]\nversion = \"fc-pvm\"\nurl = \"https://x/pvm\"\n"
        "[kernel.kvm]\nversion = \"k-kvm\"\nurl = \"https://x/kkvm\"\n"
        "[kernel.pvm]\nversion = \"k-pvm\"\nurl = \"https://x/kpvm\"\n"
        "[tools]\nversion = \"0.1\"\nurl = \"ghcr.io/x:{version}\"\n"
        "[overlaybd]\nversion = \"v1\"\npackage_url = \"https://x/{version}\"\n"
        "[regclient]\nversion = \"v0.11.5\"\nurl = \"https://x/{version}-{arch}\"\n";

    const agentenv::core::Expected<deps::Manifest, std::string> manifest =
        deps::Manifest::ParseString(toml);
    MT_EXPECT_TRUE(manifest.ok());

    MT_EXPECT_EQ(manifest.value().firecracker.ForMode(VirtualizationMode::Kvm).version,
                 std::string("fc-kvm"));
    MT_EXPECT_EQ(manifest.value().firecracker.ForMode(VirtualizationMode::Pvm).version,
                 std::string("fc-pvm"));
    MT_EXPECT_EQ(manifest.value().kernel.ForMode(VirtualizationMode::Pvm).version,
                 std::string("k-pvm"));
    MT_EXPECT_EQ(manifest.value().overlaybd.version, std::string("v1"));
    MT_EXPECT_TRUE(manifest.value().overlaybd.package_url.has_value());
}

MT_TEST(manifest_rejects_a_missing_section) {
    // A manifest without regclient would make regctl unprovisionable, so it
    // must fail loudly rather than default to an empty URL.
    const char* const toml =
        "[firecracker.kvm]\nversion = \"a\"\nurl = \"u\"\n"
        "[firecracker.pvm]\nversion = \"a\"\nurl = \"u\"\n"
        "[kernel.kvm]\nversion = \"a\"\nurl = \"u\"\n"
        "[kernel.pvm]\nversion = \"a\"\nurl = \"u\"\n"
        "[tools]\nurl = \"u\"\n"
        "[overlaybd]\nversion = \"v1\"\n";
    const agentenv::core::Expected<deps::Manifest, std::string> manifest =
        deps::Manifest::ParseString(toml);
    MT_EXPECT_TRUE(!manifest.ok());
    MT_EXPECT_TRUE(manifest.error().find("regclient") != std::string::npos);
}

// ---------------------------------------------------------------------------
// regctl version probing
// ---------------------------------------------------------------------------

MT_TEST(regctl_version_match_uses_exact_token) {
    // Rust: `regctl_version_match_uses_exact_token`.
    MT_EXPECT_TRUE(deps::VersionOutputMentionsExactToken("VCSTag:     v0.11.5\n", "v0.11.5"));
    MT_EXPECT_TRUE(deps::VersionOutputMentionsExactToken("{\"VCSTag\":\"v0.11.5\"}", "v0.11.5"));
    MT_EXPECT_TRUE(deps::VersionOutputMentionsExactToken("a,v0.11.5,b", "v0.11.5"));

    // A longer version must not match a prefix of it.
    MT_EXPECT_TRUE(!deps::VersionOutputMentionsExactToken("VCSTag: v0.11.50\n", "v0.11.5"));
    MT_EXPECT_TRUE(!deps::VersionOutputMentionsExactToken("xv0.11.5", "v0.11.5"));
    MT_EXPECT_TRUE(!deps::VersionOutputMentionsExactToken("", "v0.11.5"));
    MT_EXPECT_TRUE(!deps::VersionOutputMentionsExactToken("v0.11.5", ""));
}

// ---------------------------------------------------------------------------
// Explicit file validation
// ---------------------------------------------------------------------------

MT_TEST(validate_explicit_file_accepts_a_good_file) {
    TempRoot root;
    root.WriteFile("kernel.bin", "payload");
    MT_EXPECT_TRUE(
        deps::ValidateExplicitFile("kernel.image_path", root.Join("kernel.bin"), false).ok());
}

MT_TEST(validate_explicit_file_rejects_empty_missing_and_directories) {
    TempRoot root;

    // Missing.
    const agentenv::core::Expected<Unit, std::string> missing =
        deps::ValidateExplicitFile("kernel.image_path", root.Join("nope"), false);
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().find("validate kernel.image_path") != std::string::npos);

    // Empty: a zero-length file is what a failed download leaves behind.
    root.WriteFile("empty.bin", "");
    const agentenv::core::Expected<Unit, std::string> empty =
        deps::ValidateExplicitFile("kernel.image_path", root.Join("empty.bin"), false);
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_TRUE(empty.error().find("is an empty file") != std::string::npos);

    // A directory.
    MT_EXPECT_TRUE(fs::CreateDirAll(root.Join("dir")).ok());
    const agentenv::core::Expected<Unit, std::string> directory =
        deps::ValidateExplicitFile("kernel.image_path", root.Join("dir"), false);
    MT_EXPECT_TRUE(!directory.ok());
    MT_EXPECT_TRUE(directory.error().find("is not a regular file") != std::string::npos);
}

MT_TEST(validate_explicit_file_enforces_the_executable_bit) {
    TempRoot root;
    root.WriteFile("firecracker", "binary");
    MT_EXPECT_TRUE(fs::SetPermissions(root.Join("firecracker"), 0644).ok());

    // A non-executable binary would fail only at launch time, so it is
    // rejected here.
    const agentenv::core::Expected<Unit, std::string> not_executable =
        deps::ValidateExplicitFile("firecracker.binary_path", root.Join("firecracker"), true);
    MT_EXPECT_TRUE(!not_executable.ok());
    MT_EXPECT_TRUE(not_executable.error().find("is not executable") != std::string::npos);

    MT_EXPECT_TRUE(fs::SetPermissions(root.Join("firecracker"), 0755).ok());
    MT_EXPECT_TRUE(
        deps::ValidateExplicitFile("firecracker.binary_path", root.Join("firecracker"), true)
            .ok());
}

// ---------------------------------------------------------------------------
// Release payload discovery
// ---------------------------------------------------------------------------

MT_TEST(walkdir_finds_files_within_the_depth_limit) {
    TempRoot root;
    root.WriteFile("a.txt", "a");
    root.WriteFile("one/b.txt", "b");
    root.WriteFile("one/two/c.txt", "c");
    // Depth 4 is beyond the limit of 3.
    root.WriteFile("one/two/three/d.txt", "d");

    const agentenv::core::Expected<std::vector<std::string>, std::string> found =
        deps::WalkDir(root.path);
    MT_EXPECT_TRUE(found.ok());

    bool has_a = false, has_b = false, has_c = false, has_d = false;
    for (std::size_t i = 0; i < found.value().size(); ++i) {
        const std::string& p = found.value()[i];
        if (p.find("a.txt") != std::string::npos) has_a = true;
        if (p.find("b.txt") != std::string::npos) has_b = true;
        if (p.find("c.txt") != std::string::npos) has_c = true;
        if (p.find("d.txt") != std::string::npos) has_d = true;
    }
    MT_EXPECT_TRUE(has_a);
    MT_EXPECT_TRUE(has_b);
    MT_EXPECT_TRUE(has_c);
    // The depth limit keeps a deep or symlinked payload from being unbounded.
    MT_EXPECT_TRUE(!has_d);
}

MT_TEST(find_firecracker_binary_prefers_the_direct_path) {
    TempRoot root;
    root.WriteFile("release/firecracker", "binary");
    root.WriteFile("release/nested/firecracker-v1.2.3-x86_64", "other");

    const agentenv::core::Expected<std::string, std::string> found =
        deps::FindFirecrackerBinary(root.Join("release"));
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_EQ(found.value(), root.Join("release/firecracker"));
}

MT_TEST(find_firecracker_binary_excludes_sibling_tools) {
    TempRoot root;
    // These ship alongside firecracker in the release tarball; picking one
    // would install the wrong program under the right name.
    root.WriteFile("release/jailer-v1.2.3-x86_64", "jailer");
    root.WriteFile("release/rebase-snap-v1.2.3-x86_64", "rebase");
    root.WriteFile("release/firecracker-v1.2.3-x86_64.debug", "debug");
    root.WriteFile("release/firecracker-v1.2.3-x86_64.yaml", "yaml");
    root.WriteFile("release/firecracker-v1.2.3-x86_64", "the real one");

    const agentenv::core::Expected<std::string, std::string> found =
        deps::FindFirecrackerBinary(root.Join("release"));
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_EQ(fs::ReadToString(found.value()).value(), std::string("the real one"));
}

MT_TEST(find_firecracker_binary_reports_an_empty_payload) {
    TempRoot root;
    root.WriteFile("release/readme.txt", "nothing here");

    const agentenv::core::Expected<std::string, std::string> found =
        deps::FindFirecrackerBinary(root.Join("release"));
    MT_EXPECT_TRUE(!found.ok());
    MT_EXPECT_TRUE(found.error().find("firecracker binary not found") != std::string::npos);
}

MT_TEST(find_cpu_template_helper_is_optional_and_skips_debug) {
    TempRoot root;
    MT_EXPECT_TRUE(fs::CreateDirAll(root.Join("release")).ok());
    // Absent is not an error: the helper is optional.
    MT_EXPECT_TRUE(!deps::FindCpuTemplateHelper(root.Join("release")).has_value());

    root.WriteFile("release/cpu-template-helper.debug", "debug");
    MT_EXPECT_TRUE(!deps::FindCpuTemplateHelper(root.Join("release")).has_value());

    root.WriteFile("release/cpu-template-helper", "real");
    const Optional<std::string> found = deps::FindCpuTemplateHelper(root.Join("release"));
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(fs::ReadToString(*found).value(), std::string("real"));
}

// ---------------------------------------------------------------------------
// Tools drive installation
// ---------------------------------------------------------------------------

MT_TEST(install_explicit_tools_drive_copies_and_sets_mode) {
    TempRoot root;
    root.WriteFile("source/tools.ext4", "tools payload");

    const std::string destination = root.Join("deps/tools/0.1.1/tools.ext4");
    MT_EXPECT_TRUE(
        deps::InstallExplicitToolsDrive(root.Join("source/tools.ext4"), destination, "0.1.1")
            .ok());

    MT_EXPECT_EQ(fs::ReadToString(destination).value(), std::string("tools payload"));
    MT_EXPECT_EQ(fs::Stat(destination).value().mode, static_cast<uint32_t>(0644));

    // No temporary left behind.
    const Optional<std::string> parent = fs::Parent(destination);
    const agentenv::core::Expected<std::vector<std::string>, std::string> entries =
        fs::ReadDir(*parent);
    MT_EXPECT_TRUE(entries.ok());
    MT_EXPECT_EQ(entries.value().size(), static_cast<std::size_t>(1));
}

MT_TEST(install_explicit_tools_drive_is_idempotent_for_identical_content) {
    TempRoot root;
    root.WriteFile("source/tools.ext4", "tools payload");
    const std::string destination = root.Join("deps/tools.ext4");

    MT_EXPECT_TRUE(
        deps::InstallExplicitToolsDrive(root.Join("source/tools.ext4"), destination, "0.1.1")
            .ok());
    // A second install verifies rather than rewrites.
    MT_EXPECT_TRUE(
        deps::InstallExplicitToolsDrive(root.Join("source/tools.ext4"), destination, "0.1.1")
            .ok());
}

MT_TEST(install_explicit_tools_drive_rejects_a_changed_payload_under_one_version) {
    TempRoot root;
    root.WriteFile("source/tools.ext4", "original payload");
    const std::string destination = root.Join("deps/tools.ext4");
    MT_EXPECT_TRUE(
        deps::InstallExplicitToolsDrive(root.Join("source/tools.ext4"), destination, "0.1.1")
            .ok());

    // Same version, different bytes: serving both would make sandboxes
    // non-reproducible, so this must fail rather than overwrite.
    root.WriteFile("source/tools.ext4", "different payload");
    const agentenv::core::Expected<Unit, std::string> conflict =
        deps::InstallExplicitToolsDrive(root.Join("source/tools.ext4"), destination, "0.1.1");
    MT_EXPECT_TRUE(!conflict.ok());
    MT_EXPECT_TRUE(conflict.error().find("conflicts with the installed file") !=
                   std::string::npos);
    MT_EXPECT_TRUE(conflict.error().find("publish a new version") != std::string::npos);

    // The installed copy is untouched.
    MT_EXPECT_EQ(fs::ReadToString(destination).value(), std::string("original payload"));
}

MT_TEST(verify_installed_tools_drive_compares_content_not_size) {
    TempRoot root;
    // Same length, different bytes: a size-only check would pass.
    root.WriteFile("a.ext4", "AAAA");
    root.WriteFile("b.ext4", "BBBB");

    MT_EXPECT_TRUE(
        !deps::VerifyInstalledToolsDrive(root.Join("a.ext4"), root.Join("b.ext4"), "0.1.1").ok());
    MT_EXPECT_TRUE(
        deps::VerifyInstalledToolsDrive(root.Join("a.ext4"), root.Join("a.ext4"), "0.1.1").ok());
}

// ---------------------------------------------------------------------------
// overlaybd DownloadConfig
// ---------------------------------------------------------------------------

MT_TEST(download_config_defaults_match_rust) {
    const obd::DownloadConfig config;
    // Rust's Default is deliberately not all-zero; a zeroed struct would
    // disable throttling and retries.
    MT_EXPECT_TRUE(!config.enable);
    MT_EXPECT_EQ(config.delay, 300);
    MT_EXPECT_EQ(config.delay_extra, 30);
    MT_EXPECT_EQ(config.max_mbps, 100);
    MT_EXPECT_EQ(config.try_cnt, 5);
    MT_EXPECT_EQ(config.block_size, static_cast<uint32_t>(16 * 1024 * 1024));
    MT_EXPECT_EQ(config.concurrency, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(config.max_inflight_blocks, static_cast<std::size_t>(16));
    MT_EXPECT_EQ(config.max_concurrent_files, static_cast<std::size_t>(8));
}

MT_TEST(download_config_json_uses_the_historical_max_mbps_key) {
    obd::DownloadConfig config;
    config.enable = true;
    const std::string json = obd::DownloadConfigToJson(config).ToString();

    // `maxMBps`, not the camelCase `maxMbps` the field name would imply.
    MT_EXPECT_TRUE(json.find("\"maxMBps\"") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"delayExtra\"") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"maxInflightBlocks\"") != std::string::npos);
}

MT_TEST(download_config_json_round_trips) {
    obd::DownloadConfig config;
    config.enable = true;
    config.delay = 7;
    config.max_mbps = 0;  // 0 means unthrottled
    config.concurrency = 3;

    const agentenv::core::Expected<obd::DownloadConfig, std::string> parsed =
        obd::ParseDownloadConfig(obd::DownloadConfigToJson(config));
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value() == config);
}

MT_TEST(download_config_parse_keeps_defaults_for_missing_keys) {
    // `Json::Parse` reports an `AnyError`, unlike the config helpers' string
    // errors, so the type is spelled out rather than mirrored from them.
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> partial =
        agentenv::core::Json::Parse("{\"enable\":true}");
    MT_EXPECT_TRUE(partial.ok());

    const agentenv::core::Expected<obd::DownloadConfig, std::string> parsed =
        obd::ParseDownloadConfig(partial.value());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().enable);
    // Everything else keeps Rust's default, not zero.
    MT_EXPECT_EQ(parsed.value().delay, 300);
    MT_EXPECT_EQ(parsed.value().try_cnt, 5);
}

int main() { return microtest::RunAll(); }
