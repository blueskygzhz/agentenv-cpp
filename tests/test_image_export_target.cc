// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/target.rs and the
// `SourceRegistryRepository` half of
// src/snapshot/repository/backends/common/acr/source_image.rs.
#include <string>
#include <vector>

#include "agentenv/snapshot/image_export_target.h"
#include "agentenv/snapshot/source_registry.h"
#include "microtest.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::SnapshotId;
namespace snapshot = agentenv::snapshot;
namespace repository = agentenv::snapshot::repository;

SnapshotId FixtureId() {
    agentenv::core::Uuid uuid;
    MT_EXPECT_TRUE(
        agentenv::core::Uuid::Parse("01936f8e-72f5-7000-8000-000000000001", &uuid));
    return SnapshotId(uuid);
}

Optional<std::string> Some(const std::string& value) {
    return Optional<std::string>(value);
}

Optional<std::string> None() { return Optional<std::string>(); }

snapshot::CommittedSnapshot Committed() {
    snapshot::CommittedSnapshot committed;
    committed.runtime_versions =
        snapshot::SnapshotRuntimeVersions::New("k", "f", "e", "0.1.0");
    return committed;
}

snapshot::OverlaybdLayerRef ExternalLayer(const std::string& url) {
    snapshot::ExternalLayer layer;
    layer.digest = "sha256:a";
    layer.repo_blob_url = url;
    layer.size = 1;
    return snapshot::OverlaybdLayerRef::External(layer);
}

snapshot::OverlaybdLayerRef ManagedLayer() {
    snapshot::ManagedLayer layer;
    layer.digest = "sha256:m";
    layer.size = 1;
    return snapshot::OverlaybdLayerRef::Managed(layer);
}

snapshot::PersistedDiskImagePublication Publication(const std::string& tag,
                                                    const std::string& image_ref,
                                                    const std::string& repo_blob_url) {
    snapshot::PersistedDiskImagePublication publication;
    publication.tag = tag;
    publication.image_ref = image_ref;
    publication.manifest_digest = "sha256:manifest";
    publication.repo_blob_url = repo_blob_url;
    return publication;
}

}  // namespace

// ---------------------------------------------------------------------------
// SourceRegistryRepository
// ---------------------------------------------------------------------------

MT_TEST(source_registry_parses_the_expected_shape) {
    const repository::RepositoryResult<snapshot::SourceRegistryRepository> source =
        snapshot::SourceRegistryRepository::Parse(
            "https://registry.example/v2/ns/app/blobs");
    MT_EXPECT_TRUE(source.ok());
    MT_EXPECT_EQ(source.value().registry, std::string("registry.example"));
    // Everything between `v2` and `blobs`, so a nested path survives.
    MT_EXPECT_EQ(source.value().repository, std::string("ns/app"));
    MT_EXPECT_EQ(source.value().repo_blob_url,
                 std::string("https://registry.example/v2/ns/app/blobs"));
}

MT_TEST(source_registry_normalizes_the_default_https_port) {
    // Two spellings of one registry must normalise together, or a snapshot
    // would be published twice.
    const snapshot::SourceRegistryRepository with_port =
        snapshot::SourceRegistryRepository::Parse(
            "https://registry.example:443/v2/app/blobs")
            .value();
    const snapshot::SourceRegistryRepository without_port =
        snapshot::SourceRegistryRepository::Parse("https://registry.example/v2/app/blobs")
            .value();
    MT_EXPECT_TRUE(with_port == without_port);

    // Any other port is significant and is kept.
    MT_EXPECT_EQ(snapshot::SourceRegistryRepository::Parse(
                     "https://registry.example:5000/v2/app/blobs")
                     .value()
                     .registry,
                 std::string("registry.example:5000"));
}

MT_TEST(source_registry_requires_https_except_on_loopback) {
    // Plain HTTP would send the bearer token in the clear.
    const repository::RepositoryResult<snapshot::SourceRegistryRepository> insecure =
        snapshot::SourceRegistryRepository::Parse("http://registry.example/v2/app/blobs");
    MT_EXPECT_TRUE(!insecure.ok());
    MT_EXPECT_TRUE(insecure.error().message.find("must use https") != std::string::npos);

    // Loopback is allowed so tests can run against a local registry.
    MT_EXPECT_TRUE(
        snapshot::SourceRegistryRepository::Parse("http://127.0.0.1:5000/v2/app/blobs").ok());
    MT_EXPECT_TRUE(
        snapshot::SourceRegistryRepository::Parse("http://localhost:5000/v2/app/blobs").ok());
}

MT_TEST(source_registry_rejects_a_wrong_path_shape) {
    const char* const rejected[] = {
        "https://registry.example/app/blobs",     // no /v2
        "https://registry.example/v2/app",        // no /blobs
        "https://registry.example/v2/blobs",      // empty repository
        "https://registry.example/v2",
        "not-a-url",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!snapshot::SourceRegistryRepository::Parse(rejected[i]).ok());
    }
}

MT_TEST(source_registry_builds_api_urls_from_the_original_scheme) {
    const snapshot::SourceRegistryRepository https =
        snapshot::SourceRegistryRepository::Parse("https://registry.example/v2/ns/app/blobs")
            .value();
    MT_EXPECT_EQ(https.ImageRef("v1"), std::string("registry.example/ns/app:v1"));
    MT_EXPECT_EQ(https.UploadUrl(),
                 std::string("https://registry.example/v2/ns/app/blobs/uploads/"));
    MT_EXPECT_EQ(https.ManifestUrl("v1"),
                 std::string("https://registry.example/v2/ns/app/manifests/v1"));

    // A loopback HTTP source must not silently switch to HTTPS for the API.
    const snapshot::SourceRegistryRepository http =
        snapshot::SourceRegistryRepository::Parse("http://127.0.0.1:5000/v2/app/blobs")
            .value();
    MT_EXPECT_TRUE(http.UploadUrl().compare(0, 5, "http:") == 0);
}

// ---------------------------------------------------------------------------
// SnapshotImageTarget::Parse
// ---------------------------------------------------------------------------

MT_TEST(target_parse_splits_registry_and_repository) {
    const snapshot::SnapshotImageTarget target =
        snapshot::SnapshotImageTarget::Parse("registry.example/ns/app", Some("v1")).value();
    MT_EXPECT_EQ(target.registry, std::string("registry.example"));
    MT_EXPECT_EQ(target.repository, std::string("ns/app"));
    MT_EXPECT_EQ(target.tag, std::string("v1"));
    MT_EXPECT_EQ(target.ImageRef(), std::string("registry.example/ns/app:v1"));
}

MT_TEST(target_parse_defaults_the_tag_and_lowercases_the_registry) {
    const snapshot::SnapshotImageTarget target =
        snapshot::SnapshotImageTarget::Parse("Registry.EXAMPLE/app", None()).value();
    MT_EXPECT_EQ(target.registry, std::string("registry.example"));
    MT_EXPECT_EQ(target.tag, std::string("latest"));

    // The same `:443` normalisation as the source parser, so an explicit
    // target and an inferred one compare equal.
    MT_EXPECT_EQ(
        snapshot::SnapshotImageTarget::Parse("registry.example:443/app", None())
            .value()
            .registry,
        std::string("registry.example"));
}

MT_TEST(target_parse_rejects_a_url_or_a_digest) {
    const repository::RepositoryResult<snapshot::SnapshotImageTarget> unused =
        repository::RepositoryResult<snapshot::SnapshotImageTarget>(
            snapshot::SnapshotImageTarget());
    (void)unused;

    const char* const rejected[] = {
        "https://registry.example/app",
        "registry.example/app@sha256:abc",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const agentenv::core::Expected<snapshot::SnapshotImageTarget,
                                       snapshot::SnapshotImageTargetError>
            result = snapshot::SnapshotImageTarget::Parse(rejected[i], None());
        MT_EXPECT_TRUE(!result.ok());
        MT_EXPECT_TRUE(result.error().reason.find("URL scheme or digest") !=
                       std::string::npos);
    }
}

MT_TEST(target_parse_requires_a_registry_and_a_repository) {
    const agentenv::core::Expected<snapshot::SnapshotImageTarget,
                                   snapshot::SnapshotImageTargetError>
        no_slash = snapshot::SnapshotImageTarget::Parse("app", None());
    MT_EXPECT_TRUE(!no_slash.ok());
    MT_EXPECT_TRUE(no_slash.error().reason.find("registry and repository path") !=
                   std::string::npos);

    const char* const invalid[] = {
        "/app",                       // empty registry
        "registry.example/",          // empty repository
        "registry with space/app",
        "registry.example/App",       // uppercase component
        "registry.example/-app",      // component must start alnum
        "registry.example/ns//app",   // empty component
    };
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        MT_EXPECT_TRUE(!snapshot::SnapshotImageTarget::Parse(invalid[i], None()).ok());
    }
}

MT_TEST(validated_tag_enforces_the_oci_grammar) {
    MT_EXPECT_EQ(snapshot::ValidatedTag(None()).value(), std::string("latest"));
    MT_EXPECT_EQ(snapshot::ValidatedTag(Some("v1.2_3-x")).value(), std::string("v1.2_3-x"));
    MT_EXPECT_TRUE(snapshot::ValidatedTag(Some("_leading")).ok());

    const char* const rejected[] = {
        "",
        ".leading-dot",   // must start alnum or underscore
        "-leading-dash",
        "has space",
        "has/slash",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        MT_EXPECT_TRUE(!snapshot::ValidatedTag(Some(rejected[i])).ok());
    }
    // 128 is the limit.
    MT_EXPECT_TRUE(snapshot::ValidatedTag(Some(std::string(128, 'a'))).ok());
    MT_EXPECT_TRUE(!snapshot::ValidatedTag(Some(std::string(129, 'a'))).ok());
}

MT_TEST(is_valid_repository_checks_every_component) {
    MT_EXPECT_TRUE(snapshot::IsValidRepository("app"));
    MT_EXPECT_TRUE(snapshot::IsValidRepository("ns/team/app"));
    MT_EXPECT_TRUE(snapshot::IsValidRepository("a1.b_c-d"));

    MT_EXPECT_TRUE(!snapshot::IsValidRepository(""));
    MT_EXPECT_TRUE(!snapshot::IsValidRepository("ns//app"));
    MT_EXPECT_TRUE(!snapshot::IsValidRepository("ns/App"));
    MT_EXPECT_TRUE(!snapshot::IsValidRepository("/app"));
    MT_EXPECT_TRUE(!snapshot::IsValidRepository("app/"));
}

// ---------------------------------------------------------------------------
// resolve_snapshot_image_target
// ---------------------------------------------------------------------------

MT_TEST(resolve_prefers_an_explicit_target) {
    const snapshot::SnapshotImageTarget target =
        snapshot::ResolveSnapshotImageTarget(FixtureId(), Committed(),
                                             Some("registry.example/app"), Some("v2"))
            .value();
    MT_EXPECT_EQ(target.registry, std::string("registry.example"));
    MT_EXPECT_EQ(target.tag, std::string("v2"));
}

MT_TEST(resolve_infers_from_the_canonical_rootfs_publication) {
    snapshot::CommittedSnapshot committed = Committed();
    // An unrelated publication must not be picked over this snapshot's own.
    committed.disk_publications.push_back(Publication(
        "latest", "other.example/app:latest", "https://other.example/v2/app/blobs"));
    committed.disk_publications.push_back(
        Publication(snapshot::RootfsSnapshotImageTag(FixtureId()),
                    "registry.example/ns/app:snap", "https://registry.example/v2/ns/app/blobs"));

    const snapshot::SnapshotImageTarget target =
        snapshot::ResolveSnapshotImageTarget(FixtureId(), committed, None(), None()).value();
    MT_EXPECT_EQ(target.registry, std::string("registry.example"));
    MT_EXPECT_EQ(target.repository, std::string("ns/app"));
    // Without an explicit tag the inferred publish gets a snapshot-specific
    // tag, so it never overwrites the source image's own.
    MT_EXPECT_EQ(target.tag, std::string("snapshot-") + FixtureId().ToString());
}

MT_TEST(resolve_infers_from_external_rootfs_layers) {
    snapshot::CommittedSnapshot committed = Committed();
    committed.rootfs_layers.push_back(
        ExternalLayer("https://registry.example/v2/ns/app/blobs"));
    // Managed layers live in this repository, not a registry.
    committed.rootfs_layers.push_back(ManagedLayer());
    // OSS-managed layers carry a synthetic s3 URL that names no registry.
    committed.rootfs_layers.push_back(ExternalLayer("s3://bucket/key"));

    const snapshot::SnapshotImageTarget target =
        snapshot::ResolveSnapshotImageTarget(FixtureId(), committed, None(), Some("v3"))
            .value();
    MT_EXPECT_EQ(target.registry, std::string("registry.example"));
    MT_EXPECT_EQ(target.repository, std::string("ns/app"));
    MT_EXPECT_EQ(target.tag, std::string("v3"));
}

MT_TEST(resolve_refuses_to_guess_between_several_sources) {
    snapshot::CommittedSnapshot committed = Committed();
    committed.rootfs_layers.push_back(ExternalLayer("https://a.example/v2/app/blobs"));
    committed.rootfs_layers.push_back(ExternalLayer("https://b.example/v2/app/blobs"));

    const agentenv::core::Expected<snapshot::SnapshotImageTarget,
                                   snapshot::SnapshotImageTargetError>
        ambiguous = snapshot::ResolveSnapshotImageTarget(FixtureId(), committed, None(),
                                                         None());
    MT_EXPECT_TRUE(!ambiguous.ok());
    MT_EXPECT_TRUE(ambiguous.error().kind ==
                   snapshot::SnapshotImageTargetError::Kind::CannotInfer);
    MT_EXPECT_TRUE(ambiguous.error().reason.find("multiple external registry sources") !=
                   std::string::npos);
    // The message tells the operator how to resolve it.
    MT_EXPECT_TRUE(ambiguous.error().Message().find("--target-repository") !=
                   std::string::npos);
}

MT_TEST(resolve_reports_when_there_is_nothing_to_infer_from) {
    snapshot::CommittedSnapshot committed = Committed();
    committed.rootfs_layers.push_back(ManagedLayer());

    const agentenv::core::Expected<snapshot::SnapshotImageTarget,
                                   snapshot::SnapshotImageTargetError>
        empty =
            snapshot::ResolveSnapshotImageTarget(FixtureId(), committed, None(), None());
    MT_EXPECT_TRUE(!empty.ok());
    MT_EXPECT_TRUE(empty.error().reason.find("no external registry source") !=
                   std::string::npos);
}

MT_TEST(resolve_treats_two_spellings_of_one_registry_as_one_source) {
    snapshot::CommittedSnapshot committed = Committed();
    // Same registry, written with and without the default port.
    committed.rootfs_layers.push_back(
        ExternalLayer("https://registry.example/v2/app/blobs"));
    committed.rootfs_layers.push_back(
        ExternalLayer("https://registry.example:443/v2/app/blobs"));

    const agentenv::core::Expected<snapshot::SnapshotImageTarget,
                                   snapshot::SnapshotImageTargetError>
        resolved =
            snapshot::ResolveSnapshotImageTarget(FixtureId(), committed, None(), None());
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_EQ(resolved.value().registry, std::string("registry.example"));
}

// ---------------------------------------------------------------------------
// snapshot_managed_publication_for_target
// ---------------------------------------------------------------------------

MT_TEST(managed_publication_matches_a_normalized_target) {
    snapshot::CommittedSnapshot committed = Committed();
    committed.disk_publications.push_back(Publication(
        "v1", "registry.example/ns/app:v1", "https://registry.example:443/v2/ns/app/blobs"));

    const snapshot::SnapshotImageTarget target =
        snapshot::SnapshotImageTarget::Parse("registry.example/ns/app", Some("v1")).value();

    // The `:443` in the stored URL must not prevent the match.
    const Optional<snapshot::PersistedDiskImagePublication> found =
        snapshot::SnapshotManagedPublicationForTarget(committed, target);
    MT_EXPECT_TRUE(found.has_value());
    MT_EXPECT_EQ(found->image_ref, std::string("registry.example/ns/app:v1"));
}

MT_TEST(managed_publication_requires_the_tag_to_match) {
    snapshot::CommittedSnapshot committed = Committed();
    committed.disk_publications.push_back(Publication(
        "v1", "registry.example/ns/app:v1", "https://registry.example/v2/ns/app/blobs"));

    const snapshot::SnapshotImageTarget other_tag =
        snapshot::SnapshotImageTarget::Parse("registry.example/ns/app", Some("v2")).value();
    MT_EXPECT_TRUE(
        !snapshot::SnapshotManagedPublicationForTarget(committed, other_tag).has_value());

    const snapshot::SnapshotImageTarget other_repo =
        snapshot::SnapshotImageTarget::Parse("registry.example/ns/other", Some("v1")).value();
    MT_EXPECT_TRUE(
        !snapshot::SnapshotManagedPublicationForTarget(committed, other_repo).has_value());
}

MT_TEST(managed_publication_falls_back_to_the_recorded_image_ref) {
    snapshot::CommittedSnapshot committed = Committed();
    // An older record whose blob URL cannot be parsed still has to be
    // findable by its image ref.
    committed.disk_publications.push_back(
        Publication("v1", "registry.example/ns/app:v1", "not-a-url"));

    const snapshot::SnapshotImageTarget target =
        snapshot::SnapshotImageTarget::Parse("registry.example/ns/app", Some("v1")).value();
    MT_EXPECT_TRUE(
        snapshot::SnapshotManagedPublicationForTarget(committed, target).has_value());
}

int main() { return microtest::RunAll(); }
