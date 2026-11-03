// SPDX-License-Identifier: MIT
// Rust: src/types/image_tag.rs and src/repository/layout.rs — `mod tests`,
// plus the POSIX document store (src/repository/backends/posixfs/catalog.rs).
#include "microtest.h"

#include <string>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/image_tag.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/posixfs/catalog.h"

using namespace agentenv;
using namespace agentenv::snapshot;
using namespace agentenv::snapshot::repository;

namespace {

ImageTag Tag(const std::string& input) {
    const core::Expected<ImageTag, std::string> tag = ImageTag::Parse(input);
    MT_EXPECT_TRUE(tag.ok());
    return tag.value();
}

}  // namespace

// ---- src/types/image_tag.rs -------------------------------------------------

MT_TEST(parses_canonical_name_and_tag) {
    const char* const cases[][2] = {
        {"app", "app:latest"},
        {"app:v1", "app:v1"},
        {"my-app.web_1:V1.2-rc_3", "my-app.web_1:V1.2-rc_3"},
        {"app:_build", "app:_build"},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        MT_EXPECT_TRUE(Tag(cases[i][0]).ToString() == cases[i][1]);
    }
    const std::string long_ref = std::string(128, 'a') + ":" + std::string(128, 'b');
    MT_EXPECT_TRUE(Tag(long_ref).ToString() == long_ref);

    const ImageTag tag = Tag("my-app.web_1:V1.2-rc_3");
    MT_EXPECT_TRUE(tag.name() == ImageName::Parse("my-app.web_1").value());
    MT_EXPECT_TRUE(tag.tag() == "V1.2-rc_3");
    MT_EXPECT_TRUE(ImageTag::New(tag.name(), "v2").value().ToString() == "my-app.web_1:v2");
}

MT_TEST(rejects_invalid_names_and_tags) {
    const std::string inputs[] = {
        "",           ":v1",       "App:v1",       "team/app:v1",
        "../app",     "app..web",  "-app",         "app-",
        "app:",       "app:.v1",   "app:-v1",      "app:v1:extra",
        "app:v/1",    "sha256:../x",
        std::string(129, 'a') + ":v1",
        "app:" + std::string(129, 'b'),
    };
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        MT_EXPECT_TRUE(!ImageTag::Parse(inputs[i]).ok());
    }
    const ImageName name = ImageName::Parse("app").value();
    const char* const tags[] = {"", ".v1", "v1:extra", "v/1", "../x"};
    for (std::size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        MT_EXPECT_TRUE(!ImageTag::New(name, tags[i]).ok());
    }
    const char* const names[] = {"App", "app:v1", "team/app", ""};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        MT_EXPECT_TRUE(!ImageName::Parse(names[i]).ok());
    }
}

// ---- src/repository/layout.rs -----------------------------------------------
//
// `DocumentCollection` is not ported (nothing in C++ lists documents yet), so
// its assertions from the Rust test are left out.

MT_TEST(document_paths_match_existing_repositories) {
    const core::SnapshotId id = core::SnapshotId::Fresh();
    const std::string ids = id.ToString();
    MT_EXPECT_TRUE(DocumentKey::BuildLog(id).Path() == "snapshots/build-logs/" + ids + ".json");
    MT_EXPECT_TRUE(DocumentKey::BuildLog(id).PosixLockPath() ==
                   "snapshots/build-logs/" + ids + ".json.lock");
    MT_EXPECT_TRUE(DocumentKey::BuildCacheState().Path() == "template-build/cache-head.json");
    MT_EXPECT_TRUE(DocumentKey::BuildCacheState().PosixLockPath() ==
                   "volumes/aliases/aenv-buildkit-cache-head.lock");

    const DocumentKey record = DocumentKey::VolumeRecord("vol_test");
    MT_EXPECT_TRUE(record.Path() == "volumes/records/vol_test.json");
    MT_EXPECT_TRUE(record.PosixPath() == "volumes/records/vol_test.json");
    MT_EXPECT_TRUE(record.PosixLockPath() == "volumes/records/vol_test.lock");

    const DocumentKey alias = DocumentKey::VolumeAlias("test-data");
    MT_EXPECT_TRUE(alias.Path() == "volumes/aliases/test-data.json");
    MT_EXPECT_TRUE(alias.PosixPath() == "volumes/aliases/test-data");
    MT_EXPECT_TRUE(alias.PosixLockPath() == "volumes/aliases/test-data.lock");

    const core::SnapshotId snapshot_id = core::SnapshotId::Fresh();
    const std::string sids = snapshot_id.ToString();
    const DocumentKey snapshot = DocumentKey::SnapshotRecord(snapshot_id);
    MT_EXPECT_TRUE(snapshot.Path() == "catalog/records/" + sids + ".json");
    MT_EXPECT_TRUE(snapshot.PosixPath() == snapshot.Path());
    MT_EXPECT_TRUE(snapshot.PosixLockPath() == "catalog/records/" + sids + ".lock");

    const DocumentKey snapshot_alias =
        DocumentKey::SnapshotAlias(SnapshotAlias::Parse("base-image").value());
    MT_EXPECT_TRUE(snapshot_alias.Path() == "catalog/aliases/base-image.json");
    MT_EXPECT_TRUE(snapshot_alias.PosixPath() == "catalog/aliases/base-image");
    MT_EXPECT_TRUE(snapshot_alias.PosixLockPath() == "catalog/aliases/base-image.lock");

    const std::string digest = core::Sha256Digest(std::string("image"));
    const RepositoryResult<DocumentKey> image = DocumentKey::Image(digest);
    MT_EXPECT_TRUE(image.ok());
    MT_EXPECT_TRUE(image.value().Path() == "images/descriptions/" + digest + ".json");
    MT_EXPECT_TRUE(image.value().PosixPath() == image.value().Path());
    MT_EXPECT_TRUE(image.value().PosixLockPath() == "images/descriptions/" + digest + ".lock");
    const RepositoryResult<DocumentKey> outside = DocumentKey::Image("sha256:../../outside");
    MT_EXPECT_TRUE(!outside.ok());
    MT_EXPECT_TRUE(outside.error().kind == RepositoryErrorKind::InvalidRequest);

    const DocumentKey tag = DocumentKey::ImageTag(Tag("web"));
    MT_EXPECT_TRUE(tag.Path() == "images/tags/web/latest.json");
    MT_EXPECT_TRUE(tag.PosixPath() == tag.Path());
    MT_EXPECT_TRUE(tag.PosixLockPath() == "images/tags/web/latest.lock");

    const core::SnapshotId build_id = core::SnapshotId::Fresh();
    const DocumentKey build = DocumentKey::ImageBuild(build_id);
    MT_EXPECT_TRUE(build.Path() == "images/builds/" + build_id.ToString() + ".json");
    MT_EXPECT_TRUE(build.PosixPath() == build.Path());
    MT_EXPECT_TRUE(build.PosixLockPath() == "images/builds/" + build_id.ToString() + ".lock");
}

// ---- POSIX document store ---------------------------------------------------

MT_TEST(posixfs_documents_round_trip_update_and_delete) {
    const std::string root = core::fs::CreateTempDir("agentenv-documents-").value();
    posixfs::PosixFsCatalogStore store(root);
    const DocumentKey key = DocumentKey::ImageTag(Tag("web:v1"));

    RepositoryResult<core::Optional<std::string> > got = store.GetDocument(key);
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(!got.value().has_value());

    MT_EXPECT_TRUE(store.PutDocument(key, "first").ok());
    MT_EXPECT_TRUE(core::fs::ReadToString(core::fs::Join(root, "images/tags/web/v1.json")).value() ==
                   "first");
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(root, "images/tags/web/v1.lock")));
    got = store.GetDocument(key);
    MT_EXPECT_TRUE(got.ok() && got.value().has_value() && *got.value() == "first");

    // `update` sees the current bytes and the call returns them.
    const RepositoryResult<core::Optional<std::string> > before = store.UpdateDocument(
        key, [](const core::Optional<std::string>& current) -> RepositoryResult<DocumentChange> {
            return DocumentChange::Put(*current + "+second");
        });
    MT_EXPECT_TRUE(before.ok() && before.value().has_value() && *before.value() == "first");
    MT_EXPECT_TRUE(*store.GetDocument(key).value() == "first+second");

    // `Keep` leaves the document alone; an error from `update` is returned.
    MT_EXPECT_TRUE(store
                       .UpdateDocument(key,
                                       [](const core::Optional<std::string>&)
                                           -> RepositoryResult<DocumentChange> {
                                           return DocumentChange::Keep();
                                       })
                       .ok());
    MT_EXPECT_TRUE(*store.GetDocument(key).value() == "first+second");
    MT_EXPECT_TRUE(!store
                        .UpdateDocument(key,
                                        [](const core::Optional<std::string>&)
                                            -> RepositoryResult<DocumentChange> {
                                            return core::make_unexpected(
                                                RepositoryError::InvalidRequest("refused"));
                                        })
                        .ok());

    MT_EXPECT_TRUE(store
                       .UpdateDocument(key,
                                       [](const core::Optional<std::string>&)
                                           -> RepositoryResult<DocumentChange> {
                                           return DocumentChange::Delete();
                                       })
                       .ok());
    MT_EXPECT_TRUE(!store.GetDocument(key).value().has_value());

    // Deleting a missing document is success.
    MT_EXPECT_TRUE(store.DeleteDocument(key).ok());
    MT_EXPECT_TRUE(store.PutDocument(key, "again").ok());
    MT_EXPECT_TRUE(store.DeleteDocument(key).ok());
    MT_EXPECT_TRUE(!store.GetDocument(key).value().has_value());

    // Aliases keep their pre-shared-layout POSIX path (no `.json`).
    const DocumentKey alias = DocumentKey::VolumeAlias("data");
    MT_EXPECT_TRUE(store.PutDocument(alias, "vol_1").ok());
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(root, "volumes/aliases/data")));

    core::fs::RemoveDirAll(root);
}

MT_MAIN
