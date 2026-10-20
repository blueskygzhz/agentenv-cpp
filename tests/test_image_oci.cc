// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — `mod tests`, the pure-logic subset.
//
// One case per `#[test]` that exercises a ported function, carrying the same
// name so the two sides can be diffed by eye. Tests whose Rust counterpart
// needs a temp OCI layout on disk (`read_image_manifest_*`) are not here,
// because the function they cover reads blobs and is not part of this port;
// `select_manifest_for_platform`, which is the decision inside it, is.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/image/oci_image.h"

using namespace agentenv;
using namespace agentenv::image;

namespace {

/// Rust test helper `layer`.
OciLayerDescriptor Layer(const std::string& media_type, const std::string& digest) {
    OciLayerDescriptor out;
    out.media_type = media_type;
    out.digest = digest;
    return out;
}

/// Rust test helper `manifest_config`.
///
/// Every manifest needs one, and no test asserts on it: the config descriptor
/// is deliberately outside classification, so it is here only to build a
/// well-formed manifest.
OciManifestConfigDescriptor ManifestConfig() {
    OciManifestConfigDescriptor out;
    out.digest = "sha256:config";
    return out;
}

OciManifest Manifest(const std::vector<OciLayerDescriptor>& layers) {
    OciManifest out;
    out.config = ManifestConfig();
    out.layers = layers;
    return out;
}

OciManifestDescriptor Descriptor(const std::string& digest) {
    OciManifestDescriptor out;
    out.media_type = "application/vnd.oci.image.manifest.v1+json";
    out.digest = digest;
    return out;
}

OciManifestDescriptor DescriptorFor(const std::string& digest, const std::string& arch,
                                    const std::string& os) {
    OciManifestDescriptor out = Descriptor(digest);
    OciPlatform platform;
    platform.architecture = core::Optional<std::string>(arch);
    platform.os = core::Optional<std::string>(os);
    out.platform = core::Optional<OciPlatform>(platform);
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

ImageBaseContext ParseContext(const std::string& raw) {
    const core::Expected<ImageResolutionMetadata, std::string> parsed = ParseOciImageConfig(raw);
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value().base_context;
}

const std::string* Lookup(const std::map<std::string, std::string>& map, const std::string& key) {
    const std::map<std::string, std::string>::const_iterator found = map.find(key);
    return found == map.end() ? 0 : &found->second;
}

}  // namespace

// ---- helpers --------------------------------------------------------------

MT_TEST(host_arch_to_oci_maps_both_naming_conventions) {
    MT_EXPECT_TRUE(HostArchToOci("x86_64").value() == "amd64");
    MT_EXPECT_TRUE(HostArchToOci("amd64").value() == "amd64");
    MT_EXPECT_TRUE(HostArchToOci("aarch64").value() == "arm64");
    MT_EXPECT_TRUE(HostArchToOci("arm64").value() == "arm64");
    // An error, not a pass-through: an unmapped name would match no manifest
    // and surface as "no manifest for your platform" instead of naming the
    // real, fixable problem.
    MT_EXPECT_TRUE(!HostArchToOci("riscv64").ok());
}

MT_TEST(regctl_stderr_is_not_found_detects_http_404) {
    MT_EXPECT_TRUE(RegctlStderrIsNotFound(
        "failed to get manifest reg/repo:tag: request failed: not found [http 404]: "
        "{\"errors\":[{\"code\":\"MANIFEST_UNKNOWN\"}]}"));
    MT_EXPECT_TRUE(RegctlStderrIsNotFound(
        "failed to get blob ...: request failed: not found [http 404]: "
        "{\"errors\":[{\"code\":\"BLOB_UNKNOWN\"}]}"));
}

MT_TEST(regctl_stderr_is_not_found_ignores_network_errors) {
    // A DNS failure is not a missing image. Reporting it as 404 would send the
    // operator looking for a typo in the reference instead of at the network.
    MT_EXPECT_TRUE(!RegctlStderrIsNotFound(
        "failed to get manifest no-such-host/foo/bar:latest: Get "
        "\"https://no-such-host/v2/...\": dial tcp: lookup no-such-host: no such host"));
    // Likewise 401: the image may well exist, the credentials do not.
    MT_EXPECT_TRUE(!RegctlStderrIsNotFound("request failed: unauthorized [http 401]"));
}

MT_TEST(blob_path_for_digest_splits_on_colon) {
    MT_EXPECT_TRUE(BlobPathForDigest("/tmp/oci", "sha256:abc123").value() ==
                   "/tmp/oci/blobs/sha256/abc123");
}

MT_TEST(blob_path_for_digest_rejects_malformed_digest) {
    MT_EXPECT_TRUE(!BlobPathForDigest("/tmp/oci", "no-colon-here").ok());
}

MT_TEST(blob_path_for_digest_rejects_traversal_and_absolute_components) {
    // An absolute algorithm component would replace the base entirely, because
    // `Join` has Rust's `Path::join` semantics.
    MT_EXPECT_TRUE(!BlobPathForDigest("/tmp/oci", "/etc:passwd").ok());
    // `..` and separators in the hex component (CWE-22).
    MT_EXPECT_TRUE(
        !BlobPathForDigest("/tmp/oci", "sha256:../../../../root/.ssh/id_rsa").ok());
    MT_EXPECT_TRUE(!BlobPathForDigest("/tmp/oci", "sha256:ab/cd").ok());
    // Separators in the algorithm component.
    MT_EXPECT_TRUE(!BlobPathForDigest("/tmp/oci", "sha256/..:abcd").ok());
    // A well-formed digest still resolves under the blob store.
    const core::Expected<std::string, std::string> ok =
        BlobPathForDigest("/tmp/oci", "sha256:0123456789abcdef");
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_TRUE(ok.value().compare(0, 15, "/tmp/oci/blobs/") == 0);
}

MT_TEST(is_valid_digest_algorithm_follows_the_oci_grammar) {
    MT_EXPECT_TRUE(IsValidDigestAlgorithm("sha256"));
    MT_EXPECT_TRUE(IsValidDigestAlgorithm("multihash+base58"));
    MT_EXPECT_TRUE(IsValidDigestAlgorithm("a.b_c+d-e"));
    MT_EXPECT_TRUE(!IsValidDigestAlgorithm(""));
    // An empty component: a trailing or doubled separator.
    MT_EXPECT_TRUE(!IsValidDigestAlgorithm("sha256-"));
    MT_EXPECT_TRUE(!IsValidDigestAlgorithm("sha..256"));
    // Uppercase is outside the grammar, which is what keeps the value usable
    // as a path component on a case-insensitive filesystem.
    MT_EXPECT_TRUE(!IsValidDigestAlgorithm("SHA256"));
    MT_EXPECT_TRUE(!IsValidDigestAlgorithm("sha256/.."));
}

MT_TEST(verify_blob_digest_checks_sha256) {
    const std::string bytes = "config-blob";
    MT_EXPECT_TRUE(
        VerifyBlobDigest(core::Sha256Digest(bytes), bytes.data(), bytes.size()).ok());
    MT_EXPECT_TRUE(
        !VerifyBlobDigest(core::Sha256Digest("other"), bytes.data(), bytes.size()).ok());
    // Fails closed on an algorithm it cannot compute, rather than skipping the
    // check and reporting the blob as verified.
    MT_EXPECT_TRUE(!VerifyBlobDigest("sha512:abc", bytes.data(), bytes.size()).ok());
}

MT_TEST(verify_blob_digest_accepts_uppercase_hex) {
    // No Rust counterpart; pins the `eq_ignore_ascii_case` in the comparison.
    // Uppercase hex is legal OCI, and rejecting it would be indistinguishable
    // from rejecting a tampered blob.
    const std::string bytes = "config-blob";
    std::string digest = core::Sha256Digest(bytes);
    for (std::size_t i = 7; i < digest.size(); ++i) {
        if (digest[i] >= 'a' && digest[i] <= 'f') digest[i] = static_cast<char>(digest[i] - 32);
    }
    MT_EXPECT_TRUE(VerifyBlobDigest(digest, bytes.data(), bytes.size()).ok());
}

MT_TEST(uuid_from_layer_digest_is_stable_and_digest_specific) {
    const core::Uuid uuid = UuidFromLayerDigest("sha256:aaa");
    // Pinned vector: OCI tools v1 keeps this layer identity across runtime
    // upgrades, so a change here silently re-identifies every cached layer.
    MT_EXPECT_TRUE(uuid.ToString() == "6d4f1c33-bc1f-8f3d-89f7-46f7fecb5234");
    MT_EXPECT_TRUE(UuidFromLayerDigest("sha256:aaa") != UuidFromLayerDigest("sha256:bbb"));

    const char* const kDigests[] = {"sha256:aaa", "sha256:aab", "sha256:aac", "sha256:aad"};
    for (std::size_t i = 0; i < sizeof(kDigests) / sizeof(kDigests[0]); ++i) {
        const core::Uuid derived = UuidFromLayerDigest(kDigests[i]);
        // RFC 4122 variant and version 8 ("custom"), as stamped by
        // `Builder::from_custom_bytes`. A reader uses these to tell a derived
        // identity from a randomly generated one.
        MT_EXPECT_TRUE((derived.bytes()[8] & 0xc0) == 0x80);
        MT_EXPECT_TRUE((derived.bytes()[6] >> 4) == 8);
    }
}

// ---- image reference parsing ---------------------------------------------

MT_TEST(parse_image_ref_splits_host_and_repository) {
    const core::Expected<ParsedImageRef, std::string> parsed =
        ParseImageRef("ghcr.io/org/repo:tag");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().host == "ghcr.io");
    MT_EXPECT_TRUE(parsed.value().repository == "org/repo");
}

MT_TEST(parse_image_ref_strips_digest_from_leaf) {
    const core::Expected<ParsedImageRef, std::string> parsed =
        ParseImageRef("registry.example.com/team/app@sha256:abc0123456789");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().host == "registry.example.com");
    MT_EXPECT_TRUE(parsed.value().repository == "team/app");
}

MT_TEST(parse_image_ref_rewrites_docker_io_for_wire_use) {
    // `docker.io` has no `/v2/` endpoint; `registry-1.docker.io` is the host
    // that serves it.
    const core::Expected<ParsedImageRef, std::string> parsed =
        ParseImageRef("docker.io/library/debian:bookworm-slim");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().host == "registry-1.docker.io");
    MT_EXPECT_TRUE(parsed.value().repository == "library/debian");
}

MT_TEST(parse_image_ref_preserves_registry_port) {
    const core::Expected<ParsedImageRef, std::string> parsed =
        ParseImageRef("registry.internal:5000/team/app:v1");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().host == "registry.internal:5000");
    MT_EXPECT_TRUE(parsed.value().repository == "team/app");
}

MT_TEST(parse_image_ref_rejects_shortnames) {
    // Rejected rather than expanded. Expansion is `ImageRefCandidates`' job
    // and depends on operator config; doing it again here would reintroduce an
    // implicit Docker Hub default below the layer configured to forbid it.
    MT_EXPECT_TRUE(!ParseImageRef("ubuntu:24.04").ok());
    MT_EXPECT_TRUE(!ParseImageRef("library/debian:tag").ok());
}

MT_TEST(image_ref_with_digest_replaces_tag_and_keeps_host) {
    MT_EXPECT_TRUE(
        ImageRefWithDigest("docker.io/library/debian:bookworm-slim", "sha256:deadbeef").value() ==
        "docker.io/library/debian@sha256:deadbeef");
    MT_EXPECT_TRUE(
        ImageRefWithDigest("registry.internal:5000/team/app@sha256:old", "sha256:new").value() ==
        "registry.internal:5000/team/app@sha256:new");
}

MT_TEST(image_ref_repository_strips_tag_and_digest) {
    MT_EXPECT_TRUE(ImageRefRepository("ghcr.io/org/repo:tag").value() == "ghcr.io/org/repo");
    MT_EXPECT_TRUE(ImageRefRepository("cr.example.com/team/app@sha256:abc").value() ==
                   "cr.example.com/team/app");
    // The host's port colon survives, because the split happens in the leaf.
    MT_EXPECT_TRUE(ImageRefRepository("registry.internal:5000/team/app:v1").value() ==
                   "registry.internal:5000/team/app");
    MT_EXPECT_TRUE(!ImageRefRepository("no-repository-segment").ok());
}

// ---- image config parsing ------------------------------------------------

MT_TEST(parse_oci_image_config_extracts_base_context) {
    const std::string raw =
        "{\n"
        "  \"architecture\": \"amd64\",\n"
        "  \"os\": \"linux\",\n"
        "  \"config\": {\n"
        "    \"Env\": [\n"
        "      \"NODE_ENV=production\",\n"
        "      \"PATH=/custom/bin:/usr/bin\",\n"
        "      \"FIRST=1\",\n"
        "      \"INVALID\",\n"
        "      \"=missing\",\n"
        "      \"FIRST=2\",\n"
        "      \"TOKEN=value=with=equals\"\n"
        "    ],\n"
        "    \"WorkingDir\": \"/workspace\"\n"
        "  }\n"
        "}";

    const ImageBaseContext context = ParseContext(raw);
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "NODE_ENV") == "production");
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "PATH") == "/custom/bin:/usr/bin");
    // Last wins, Docker-compatibly.
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "FIRST") == "2");
    // Split on the *first* `=` only, so a value may contain more.
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "TOKEN") == "value=with=equals");
    // No `=` at all: skipped rather than stored with an empty value.
    MT_EXPECT_TRUE(Lookup(context.env_vars, "INVALID") == 0);
    // `=missing` would insert a nameless variable, which `execve` rejects for
    // the whole environment rather than just that entry.
    MT_EXPECT_TRUE(Lookup(context.env_vars, "") == 0);
    MT_EXPECT_TRUE(context.workdir.has_value() && *context.workdir == "/workspace");
}

MT_TEST(parse_oci_image_config_extracts_all_runtime_fields) {
    // The canonical example from the OCI image-spec config.md.
    const std::string raw =
        "{\n"
        "  \"architecture\": \"amd64\",\n"
        "  \"os\": \"linux\",\n"
        "  \"config\": {\n"
        "    \"User\": \"alice\",\n"
        "    \"ExposedPorts\": {\"8080/tcp\": {}, \"443/tcp\": {}},\n"
        "    \"Env\": [\n"
        "      \"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\",\n"
        "      \"FOO=oci_is_a\",\n"
        "      \"BAR=well_written_spec\"\n"
        "    ],\n"
        "    \"Entrypoint\": [\"/bin/my-app-binary\"],\n"
        "    \"Cmd\": [\"--foreground\", \"--config\", \"/etc/my-app.d/default.cfg\"],\n"
        "    \"Volumes\": {\"/var/job-result-data\": {}, \"/var/log/my-app-logs\": {}},\n"
        "    \"WorkingDir\": \"/home/alice\",\n"
        "    \"Labels\": {\n"
        "      \"com.example.project.git.url\": \"https://example.com/project.git\",\n"
        "      \"com.example.project.git.commit\": \"45a939b2999782a3f005621a8d0f29aa387e1d6b\"\n"
        "    }\n"
        "  }\n"
        "}";

    const ImageBaseContext context = ParseContext(raw);
    MT_EXPECT_TRUE(context.user.has_value() && *context.user == "alice");
    MT_EXPECT_TRUE(context.workdir.has_value() && *context.workdir == "/home/alice");
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "FOO") == "oci_is_a");
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "BAR") == "well_written_spec");

    MT_EXPECT_TRUE(context.entrypoint.has_value());
    MT_EXPECT_TRUE(context.entrypoint->size() == 1);
    MT_EXPECT_TRUE((*context.entrypoint)[0] == "/bin/my-app-binary");

    MT_EXPECT_TRUE(context.cmd.has_value());
    MT_EXPECT_TRUE(context.cmd->size() == 3);
    MT_EXPECT_TRUE((*context.cmd)[2] == "/etc/my-app.d/default.cfg");

    // Sorted, not insertion-ordered: the keys come from an ordered map here
    // where Rust uses a `HashMap` and sorts in the test. Sorted output meets
    // the same contract and additionally makes the metadata byte-stable.
    MT_EXPECT_TRUE(context.exposed_ports.size() == 2);
    MT_EXPECT_TRUE(context.exposed_ports[0] == "443/tcp");
    MT_EXPECT_TRUE(context.exposed_ports[1] == "8080/tcp");

    MT_EXPECT_TRUE(context.volumes.size() == 2);
    MT_EXPECT_TRUE(context.volumes[0] == "/var/job-result-data");
    MT_EXPECT_TRUE(context.volumes[1] == "/var/log/my-app-logs");

    MT_EXPECT_TRUE(*Lookup(context.labels, "com.example.project.git.url") ==
                   "https://example.com/project.git");
    MT_EXPECT_TRUE(*Lookup(context.labels, "com.example.project.git.commit") ==
                   "45a939b2999782a3f005621a8d0f29aa387e1d6b");
}

MT_TEST(parse_oci_image_config_tolerates_missing_config) {
    const core::Expected<ImageResolutionMetadata, std::string> parsed =
        ParseOciImageConfig("{\"architecture\":\"amd64\"}");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().base_context == ImageBaseContext());
    // No `config` key in the document, so there is nothing to pass through.
    MT_EXPECT_TRUE(!parsed.value().raw_config.has_value());
}

MT_TEST(parse_oci_image_config_tolerates_null_map_fields) {
    const std::string raw =
        "{\n"
        "  \"architecture\": \"arm64\",\n"
        "  \"os\": \"linux\",\n"
        "  \"config\": {\n"
        "    \"Env\": [\"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"],\n"
        "    \"Cmd\": [\"/bin/bash\"],\n"
        "    \"ExposedPorts\": null,\n"
        "    \"Volumes\": null,\n"
        "    \"Labels\": null\n"
        "  }\n"
        "}";

    const ImageBaseContext context = ParseContext(raw);
    MT_EXPECT_TRUE(*Lookup(context.env_vars, "PATH") ==
                   "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    MT_EXPECT_TRUE(context.cmd.has_value() && context.cmd->size() == 1);
    MT_EXPECT_TRUE((*context.cmd)[0] == "/bin/bash");
    // An explicit null takes the default instead of failing, matching
    // `#[serde(default)]` on an `Option`: real registries emit these.
    MT_EXPECT_TRUE(context.exposed_ports.empty());
    MT_EXPECT_TRUE(context.volumes.empty());
    MT_EXPECT_TRUE(context.labels.empty());
}

MT_TEST(parse_oci_image_config_keeps_only_the_config_object_as_raw_config) {
    // No Rust counterpart, but this is the one property of `raw_config` that a
    // consumer depends on: it is handed on as "the container config", so
    // `created`/`architecture`/`rootfs` must not be inside it.
    const core::Expected<ImageResolutionMetadata, std::string> parsed = ParseOciImageConfig(
        "{\"architecture\":\"amd64\",\"created\":\"2024-01-01T00:00:00Z\","
        "\"config\":{\"User\":\"root\"}}");
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(parsed.value().raw_config.has_value());
    const std::string text = parsed.value().raw_config->ToString();
    MT_EXPECT_TRUE(Contains(text, "root"));
    MT_EXPECT_TRUE(!Contains(text, "architecture"));
    MT_EXPECT_TRUE(!Contains(text, "created"));
}

MT_TEST(parse_oci_image_config_rejects_malformed_json) {
    MT_EXPECT_TRUE(!ParseOciImageConfig("{not json").ok());
    // Wrong *types* are rejected too, matching serde: a string where an array
    // belongs would otherwise yield an empty entrypoint and start the
    // container with the wrong command.
    MT_EXPECT_TRUE(!ParseOciImageConfig("{\"config\":{\"Entrypoint\":\"/bin/sh\"}}").ok());
    MT_EXPECT_TRUE(!ParseOciImageConfig("{\"config\":{\"Env\":[1,2]}}").ok());
    MT_EXPECT_TRUE(!ParseOciImageConfig("{\"config\":{\"Labels\":{\"a\":1}}}").ok());
}

// ---- platform selection --------------------------------------------------

MT_TEST(select_manifest_for_platform_returns_only_entry_when_single) {
    // Accepted whatever its platform says: that is what a single-platform
    // image looks like, and a registry may omit the platform block there.
    std::vector<OciManifestDescriptor> manifests;
    manifests.push_back(Descriptor("sha256:aaa"));
    const core::Expected<std::size_t, std::string> picked =
        SelectManifestForPlatform(manifests, "amd64", "linux");
    MT_EXPECT_TRUE(picked.ok());
    MT_EXPECT_TRUE(manifests[picked.value()].digest == "sha256:aaa");
}

MT_TEST(select_manifest_for_platform_picks_matching_arch) {
    std::vector<OciManifestDescriptor> manifests;
    manifests.push_back(DescriptorFor("sha256:amd", "amd64", "linux"));
    manifests.push_back(DescriptorFor("sha256:arm", "arm64", "linux"));
    const core::Expected<std::size_t, std::string> picked =
        SelectManifestForPlatform(manifests, "arm64", "linux");
    MT_EXPECT_TRUE(picked.ok());
    MT_EXPECT_TRUE(manifests[picked.value()].digest == "sha256:arm");
}

MT_TEST(select_manifest_for_platform_ignores_platformless_entries_when_multi_arch) {
    // A platformless entry in a multi-entry index carries no claim about the
    // host, so matching it would be a guess; the explicit entry wins even
    // though it comes second.
    std::vector<OciManifestDescriptor> manifests;
    manifests.push_back(Descriptor("sha256:no-platform"));
    manifests.push_back(DescriptorFor("sha256:amd", "amd64", "linux"));
    const core::Expected<std::size_t, std::string> picked =
        SelectManifestForPlatform(manifests, "amd64", "linux");
    MT_EXPECT_TRUE(picked.ok());
    MT_EXPECT_TRUE(manifests[picked.value()].digest == "sha256:amd");
}

MT_TEST(select_manifest_for_platform_errors_with_available_list_when_no_match) {
    std::vector<OciManifestDescriptor> manifests;
    manifests.push_back(DescriptorFor("sha256:amd", "amd64", "linux"));
    manifests.push_back(DescriptorFor("sha256:arm", "arm64", "linux"));
    const core::Expected<std::size_t, std::string> picked =
        SelectManifestForPlatform(manifests, "riscv64", "linux");
    MT_EXPECT_TRUE(!picked.ok());
    // The list is part of the message: "no manifest matches linux/riscv64"
    // alone does not say whether the image is single-arch, built for another
    // OS, or published without platform blocks.
    MT_EXPECT_TRUE(Contains(picked.error(), "linux/amd64"));
    MT_EXPECT_TRUE(Contains(picked.error(), "linux/arm64"));
}

MT_TEST(select_manifest_for_platform_names_platformless_entries_in_the_error) {
    // No Rust counterpart; covers the `no-platform` branch of the same list.
    std::vector<OciManifestDescriptor> manifests;
    manifests.push_back(Descriptor("sha256:one"));
    manifests.push_back(Descriptor("sha256:two"));
    const core::Expected<std::size_t, std::string> picked =
        SelectManifestForPlatform(manifests, "amd64", "linux");
    MT_EXPECT_TRUE(!picked.ok());
    MT_EXPECT_TRUE(Contains(picked.error(), "no-platform"));
}

// ---- layer classification ------------------------------------------------

MT_TEST(classify_layer_flags_overlaybd_native_media_types) {
    MT_EXPECT_TRUE(
        ClassifyLayer(Layer("application/vnd.containerd.overlaybd.image.layer.v1.zfile",
                            "sha256:aaa")) == LayerClass::OverlaybdNative);
    MT_EXPECT_TRUE(ClassifyLayer(Layer("application/vnd.containerd.overlaybd.v1.tar",
                                       "sha256:aaa")) == LayerClass::OverlaybdNative);
}

MT_TEST(classify_layer_recognizes_standard_tar_types) {
    const char* const kMediaTypes[] = {
        "application/vnd.oci.image.layer.v1.tar",
        "application/vnd.oci.image.layer.v1.tar+gzip",
        "application/vnd.oci.image.layer.v1.tar+zstd",
        "application/vnd.docker.image.rootfs.diff.tar",
        "application/vnd.docker.image.rootfs.diff.tar.gzip",
    };
    for (std::size_t i = 0; i < sizeof(kMediaTypes) / sizeof(kMediaTypes[0]); ++i) {
        MT_EXPECT_TRUE(ClassifyLayer(Layer(kMediaTypes[i], "sha256:aaa")) ==
                       LayerClass::StandardTar);
    }
}

MT_TEST(classify_layer_treats_tar_with_self_referential_overlaybd_annotation_as_native) {
    const std::string digest =
        "sha256:4cafc55d878a0f1f2fb497369b138272eda40a8c66b7c922f0693b89cff6b0f0";
    OciLayerDescriptor layer = Layer("application/vnd.oci.image.layer.v1.tar", digest);
    // `blob-digest == own digest` is the publisher saying "these bytes ARE the
    // overlaybd content"; the tar mediaType is only there for OCI clients.
    layer.annotations[kOverlaybdBlobDigestAnnotation] = digest;
    layer.annotations["containerd.io/snapshot/overlaybd/blob-size"] = "47760418";
    MT_EXPECT_TRUE(ClassifyLayer(layer) == LayerClass::OverlaybdNative);
}

MT_TEST(classify_layer_flags_tar_wrapped_overlaybd_when_annotation_points_at_inner_blob) {
    OciLayerDescriptor layer =
        Layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:outerbeef");
    // A *different* digest names an inner blob, so the wire bytes are a tar
    // that still needs local extraction.
    layer.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:innerbeef";
    MT_EXPECT_TRUE(ClassifyLayer(layer) == LayerClass::OverlaybdTarWrapped);
}

MT_TEST(classify_layer_returns_unknown_for_unrecognized_media_types) {
    MT_EXPECT_TRUE(ClassifyLayer(Layer("application/vnd.something.exotic.v1", "sha256:aaa")) ==
                   LayerClass::Unknown);
}

MT_TEST(classify_layer_turbo_annotation_wins_over_self_referential_digest) {
    // Turbo layers also carry `blob-digest == layer digest`, so without the
    // precedence they would classify as native and fail deep in the runtime
    // with a short-read zfile probe instead of being rejected here.
    OciLayerDescriptor layer = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:aaa");
    layer.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:aaa";
    layer.annotations[std::string(kOverlaybdTurboAnnotationPrefix) + "target-digest"] =
        "sha256:original";
    MT_EXPECT_TRUE(ClassifyLayer(layer) == LayerClass::OverlaybdTurbo);
}

// ---- manifest classification ---------------------------------------------

MT_TEST(classify_manifest_recognizes_standard_oci_image) {
    std::vector<OciLayerDescriptor> layers;
    layers.push_back(Layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:a"));
    layers.push_back(Layer("application/vnd.oci.image.layer.v1.tar+zstd", "sha256:b"));
    layers.push_back(Layer("application/vnd.docker.image.rootfs.diff.tar.gzip", "sha256:c"));
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(Manifest(layers));
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.value() == ImageFormat::StandardOci);
}

MT_TEST(classify_manifest_recognizes_overlaybd_native_image) {
    std::vector<OciLayerDescriptor> layers;
    const char* const kZfile = "application/vnd.containerd.overlaybd.image.layer.v1.zfile";
    layers.push_back(Layer(kZfile, "sha256:a"));
    layers.push_back(Layer(kZfile, "sha256:b"));
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(Manifest(layers));
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.value() == ImageFormat::OverlaybdNative);
}

MT_TEST(classify_manifest_rejects_mixed_standard_and_overlaybd_layers) {
    std::vector<OciLayerDescriptor> layers;
    layers.push_back(Layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:a"));
    layers.push_back(
        Layer("application/vnd.containerd.overlaybd.image.layer.v1.zfile", "sha256:b"));
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(Manifest(layers));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "mix of standard OCI and overlaybd"));
}

MT_TEST(classify_manifest_rejects_tar_wrapped_overlaybd) {
    OciLayerDescriptor layer =
        Layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:a");
    layer.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:deadbeef";
    const core::Expected<ImageFormat, ImageError> got =
        ClassifyManifest(Manifest(std::vector<OciLayerDescriptor>(1, layer)));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "tar-wrapped overlaybd"));
    // The annotations are in the message: otherwise the operator is told the
    // layer is tar-wrapped but not which annotation said so.
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "sha256:deadbeef"));
}

MT_TEST(classify_manifest_recognizes_acr_mediatype_lying_overlaybd_image) {
    // ACR / accelerated-container-image publishes each layer with a tar
    // mediaType plus a blob-digest annotation equal to the layer's own digest.
    // The whole image must classify as native so it takes the registryfs_v2
    // passthrough rather than a conversion.
    const std::string first =
        "sha256:4cafc55d878a0f1f2fb497369b138272eda40a8c66b7c922f0693b89cff6b0f0";
    OciLayerDescriptor l0 = Layer("application/vnd.oci.image.layer.v1.tar", first);
    l0.annotations[kOverlaybdBlobDigestAnnotation] = first;
    l0.annotations["containerd.io/snapshot/overlaybd/blob-size"] = "47760418";
    OciLayerDescriptor l1 = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:bbbbbbbb");
    l1.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:bbbbbbbb";

    std::vector<OciLayerDescriptor> layers;
    layers.push_back(l0);
    layers.push_back(l1);
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(Manifest(layers));
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.value() == ImageFormat::OverlaybdNative);
}

MT_TEST(classify_manifest_rejects_unknown_media_type) {
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(
        Manifest(std::vector<OciLayerDescriptor>(1, Layer("application/vnd.something.odd",
                                                          "sha256:a"))));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "unsupported mediaType"));
    // 4xx, not 5xx: this is a property of the image that was asked for, and
    // retrying it will never succeed.
    MT_EXPECT_TRUE(got.error().IsUserError());
    MT_EXPECT_TRUE(got.error().kind == ImageError::kUnsupportedImage);
}

MT_TEST(classify_manifest_rejects_turbo_oci_by_artifact_type) {
    // Authoritative before any layer is inspected.
    OciLayerDescriptor layer = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:a");
    layer.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:a";
    OciManifest manifest = Manifest(std::vector<OciLayerDescriptor>(1, layer));
    manifest.artifact_type = core::Optional<std::string>(kOverlaybdTurboArtifactType);
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(manifest);
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "turbo-OCI"));
    MT_EXPECT_TRUE(got.error().kind == ImageError::kUnsupportedImage);
}

MT_TEST(classify_manifest_rejects_turbo_oci_by_layer_annotations) {
    OciLayerDescriptor turbo = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:a");
    turbo.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:a";
    turbo.annotations[std::string(kOverlaybdTurboAnnotationPrefix) + "target-digest"] =
        "sha256:original";
    turbo.annotations[kOverlaybdVersionAnnotation] = "0.1.0-turbo.ociv1";
    OciLayerDescriptor native = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:b");
    native.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:b";
    native.annotations[kOverlaybdVersionAnnotation] = "0.1.0";

    std::vector<OciLayerDescriptor> layers;
    layers.push_back(turbo);
    layers.push_back(native);
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(Manifest(layers));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(got.error().kind == ImageError::kUnsupportedImage);
    MT_EXPECT_TRUE(got.error().IsUserError());
    MT_EXPECT_TRUE(Contains(got.error().ToString(), "turbo-OCI"));
}

MT_TEST(classify_manifest_rejects_turbo_oci_by_version_annotation_only) {
    OciLayerDescriptor layer = Layer("application/vnd.oci.image.layer.v1.tar", "sha256:a");
    layer.annotations[kOverlaybdBlobDigestAnnotation] = "sha256:a";
    layer.annotations[kOverlaybdVersionAnnotation] = "0.1.0-turbo.ociv1";
    const core::Expected<ImageFormat, ImageError> got =
        ClassifyManifest(Manifest(std::vector<OciLayerDescriptor>(1, layer)));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(got.error().kind == ImageError::kUnsupportedImage);
}

MT_TEST(classify_manifest_rejects_empty_layers_as_a_server_error) {
    // No Rust counterpart, and the one case that is deliberately *not* a user
    // error: a layerless manifest is not a shape a publisher chose, so it maps
    // to 5xx rather than telling the caller to fix their reference.
    const core::Expected<ImageFormat, ImageError> got = ClassifyManifest(OciManifest());
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(got.error().kind == ImageError::kOther);
    MT_EXPECT_TRUE(!got.error().IsUserError());
}

MT_TEST(image_format_to_string_matches_the_rust_display_impl) {
    // These exact strings are emitted as metric labels and log fields, so they
    // are observable surface and cannot be prettified.
    MT_EXPECT_TRUE(std::string(ImageFormatToString(ImageFormat::StandardOci)) == "standard_oci");
    MT_EXPECT_TRUE(std::string(ImageFormatToString(ImageFormat::OverlaybdNative)) ==
                   "overlaybd_native");
}

MT_MAIN
