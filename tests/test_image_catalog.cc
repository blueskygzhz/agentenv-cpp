// SPDX-License-Identifier: MIT
// Rust: src/image/catalog.rs — exercised the way the resolver test
// `image_tags_resolve_to_the_digest_they_point_to_now` and `get_image`'s
// tamper check use it, over a POSIX repository in a temp directory.
#include "microtest.h"

#include <memory>
#include <string>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/image/catalog.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"

using namespace agentenv;
using namespace agentenv::image;
namespace repo = agentenv::snapshot::repository;

namespace {

struct TempRepository {
    std::string root;
    std::unique_ptr<repo::posixfs::PosixFsSnapshotRepository> repository;

    TempRepository() {
        root = core::fs::CreateTempDir("agentenv-image-catalog-").value();
        repository.reset(new repo::posixfs::PosixFsSnapshotRepository(
            std::make_shared<repo::posixfs::PosixFsCatalogStore>(root)));
    }
    ~TempRepository() { core::fs::RemoveDirAll(root); }
};

PublishedImage Image(const std::string& cmd) {
    snapshot::ManagedLayer layer;
    layer.digest = core::Sha256Digest(std::string("base-layer"));
    layer.size = 10;
    core::Json config = core::Json(core::JsonObject());
    config["Cmd"] = core::Json(core::JsonArray(1, core::Json(cmd)));
    return PublishedImage::New(
        "amd64", std::vector<snapshot::OverlaybdLayerRef>(1, snapshot::OverlaybdLayerRef::Managed(layer)),
        config);
}

snapshot::ImageTag Tag(const std::string& input) { return snapshot::ImageTag::Parse(input).value(); }

}  // namespace

MT_TEST(image_tag_record_encodes_camel_case_in_declaration_order) {
    ImageTagRecord record;
    record.image_digest = "sha256:abc";
    record.created_at_unix_ms = 42;
    MT_EXPECT_TRUE(record.Encode() == "{\"imageDigest\":\"sha256:abc\",\"createdAtUnixMs\":42}");
    const core::Expected<ImageTagRecord, std::string> decoded =
        ImageTagRecord::Decode("{\"createdAtUnixMs\":42,\"imageDigest\":\"sha256:abc\",\"x\":1}");
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(decoded.value() == record);
    MT_EXPECT_TRUE(!ImageTagRecord::Decode("{\"imageDigest\":\"sha256:abc\"}").ok());
    MT_EXPECT_TRUE(!ImageTagRecord::Decode("\"not-a-digest\"").ok());
}

MT_TEST(published_images_round_trip_and_are_verified_on_read) {
    TempRepository t;
    const repo::RepositoryResult<std::string> digest = PutImage(*t.repository, Image("v1"));
    MT_EXPECT_TRUE(digest.ok());
    MT_EXPECT_TRUE(digest.value() == Image("v1").Encode().value().first);
    // Idempotent: the digest names the complete description.
    MT_EXPECT_TRUE(PutImage(*t.repository, Image("v1")).value() == digest.value());

    const repo::RepositoryResult<core::Optional<PublishedImage> > got =
        GetImage(*t.repository, digest.value());
    MT_EXPECT_TRUE(got.ok() && got.value().has_value());
    MT_EXPECT_TRUE(got.value()->config["Cmd"].as_array()[0].as_string() == "v1");

    const std::string absent = core::Sha256Digest(std::string("absent"));
    MT_EXPECT_TRUE(!GetImage(*t.repository, absent).value().has_value());
    MT_EXPECT_TRUE(!GetImage(*t.repository, "sha256:short").ok());

    // Tampering with the stored description fails the digest check.
    const std::string path =
        core::fs::Join(t.root, "images/descriptions/" + digest.value() + ".json");
    core::Json description = core::Json::Parse(core::fs::ReadToString(path).value()).value();
    description["config"]["Cmd"] = core::Json(core::JsonArray(1, core::Json("tampered")));
    MT_EXPECT_TRUE(core::fs::Write(path, description.ToString()).ok());
    const repo::RepositoryResult<core::Optional<PublishedImage> > tampered =
        GetImage(*t.repository, digest.value());
    MT_EXPECT_TRUE(!tampered.ok());
    MT_EXPECT_TRUE(tampered.error().ToString().find("digest mismatch") != std::string::npos);

    // An invalid description is refused before anything is written.
    PublishedImage invalid = Image("v1");
    invalid.layers.clear();
    MT_EXPECT_TRUE(!PutImage(*t.repository, invalid).ok());
}

MT_TEST(image_tags_point_at_the_digest_they_were_last_moved_to) {
    TempRepository t;
    const std::string first = PutImage(*t.repository, Image("v1")).value();
    const std::string second = PutImage(*t.repository, Image("v2")).value();
    const snapshot::ImageTag tag = Tag("web:v1");

    MT_EXPECT_TRUE(!GetImageTag(*t.repository, tag).value().has_value());
    MT_EXPECT_TRUE(PutImageTag(*t.repository, tag, first).ok());
    MT_EXPECT_TRUE(*GetImageTag(*t.repository, tag).value() == first);
    MT_EXPECT_TRUE(PutImageTag(*t.repository, tag, second).ok());
    MT_EXPECT_TRUE(*GetImageTag(*t.repository, tag).value() == second);

    const repo::RepositoryResult<core::Unit> short_digest =
        PutImageTag(*t.repository, tag, "sha256:short");
    MT_EXPECT_TRUE(!short_digest.ok());
    MT_EXPECT_TRUE(short_digest.error().kind == repo::RepositoryErrorKind::InvalidRequest);
    MT_EXPECT_TRUE(*GetImageTag(*t.repository, tag).value() == second);

    const std::string path = core::fs::Join(t.root, "images/tags/web/v1.json");
    MT_EXPECT_TRUE(core::fs::Write(path, "\"not-a-digest\"").ok());
    const repo::RepositoryResult<core::Optional<std::string> > corrupt =
        GetImageTag(*t.repository, tag);
    MT_EXPECT_TRUE(!corrupt.ok());
    MT_EXPECT_TRUE(corrupt.error().kind == repo::RepositoryErrorKind::Backend);
    MT_EXPECT_TRUE(corrupt.error().ToString().find("image tag 'web:v1' does not hold an image digest") !=
                   std::string::npos);
}

MT_TEST(repositories_without_documents_report_unsupported) {
    // The base-class default, as Rust's trait default body.
    struct Bare : repo::SnapshotRepository {
        repo::RepositoryResult<repo::SnapshotRecord> Create(const repo::SnapshotRecord&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<core::Optional<repo::SnapshotRecord> > Get(
            const std::string&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
            const repo::SnapshotListFilter&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<core::Unit> Delete(const std::string&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(
            const std::string&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(const core::SnapshotId&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
        repo::RepositoryResult<core::Unit> MarkBuildError(
            const core::SnapshotId&, const repo::TemplateBuildErrorReason&) override {
            return core::make_unexpected(repo::RepositoryError::Unsupported("x"));
        }
    } bare;
    const repo::RepositoryResult<core::Optional<std::string> > got =
        GetImageTag(bare, Tag("web"));
    MT_EXPECT_TRUE(!got.ok());
    MT_EXPECT_TRUE(got.error().kind == repo::RepositoryErrorKind::Unsupported);
}

MT_MAIN
