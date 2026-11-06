// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/service.rs
//
// End-to-end against the fake `regctl` executable, so the assertions are about
// what reached the registry rather than about which methods were called. The
// fake hashes every blob and manifest it stores, which makes "the right bytes
// arrived" checkable instead of assumed.
//
// The transfer-shape choices get the most attention here. Picking wrongly
// between mount, pipe and upload is not a correctness failure -- the blob ends
// up in the right place either way -- so only an assertion on the *argv* can
// catch a regression that quietly starts moving gigabytes over the network.
#include "agentenv/snapshot/image_export/service.h"

#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/snapshot/repository/blobs.h"
#include "fake_regctl.h"
#include "microtest.h"

using namespace agentenv;                          // NOLINT
using namespace agentenv::snapshot;                // NOLINT
using namespace agentenv::snapshot::image_export;  // NOLINT
using namespace agentenv::snapshot::repository;     // NOLINT

namespace {

const char* kTargetRepo = "reg.example/team/newapp";
const char* kTargetRef = "reg.example/team/newapp:snap-1";
const char* kSrcUrl = "https://reg.example/v2/team/app/blobs";
const char* kCrossUrl = "https://other.example/v2/team/app/blobs";

std::string DigestOf(const std::string& bytes) { return core::Sha256Digest(bytes); }

OverlaybdLayerRef ManagedRef(const std::string& bytes) {
    ManagedLayer layer;
    layer.digest = DigestOf(bytes);
    layer.size = bytes.size();
    return OverlaybdLayerRef::Managed(layer);
}

OverlaybdLayerRef ExternalRef(const std::string& bytes, const std::string& url) {
    ExternalLayer layer;
    layer.digest = DigestOf(bytes);
    layer.size = bytes.size();
    layer.repo_blob_url = url;
    return OverlaybdLayerRef::External(layer);
}

core::SnapshotId SomeId() {
    const core::Expected<core::SnapshotId, std::string> id =
        core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

/// A repository that refuses everything: these tests drive
/// `PublishCommittedRootfs` directly, so a stray record read is a bug worth
/// surfacing rather than papering over.
struct RefusingRepository : public SnapshotRepository {
    core::Optional<SnapshotRecord> record;

    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string&) override {
        return record;
    }
    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord&) override {
        return core::make_unexpected(RepositoryError::Unsupported("Create"));
    }
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter&) override {
        return core::make_unexpected(RepositoryError::Unsupported("List"));
    }
    RepositoryResult<core::Unit> Delete(const std::string&) override {
        return core::make_unexpected(RepositoryError::Unsupported("Delete"));
    }
    RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(
        const std::string&) override {
        return core::make_unexpected(RepositoryError::Unsupported("ResolveAlias"));
    }
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId&) override {
        return core::make_unexpected(RepositoryError::Unsupported("TryStartBuild"));
    }
    RepositoryResult<core::Unit> MarkBuildError(const core::SnapshotId&,
                                                const TemplateBuildErrorReason&) override {
        return core::make_unexpected(RepositoryError::Unsupported("MarkBuildError"));
    }
    RepositoryResult<core::Unit> DeleteSnapshotBlobs(const core::SnapshotId&) override {
        return core::make_unexpected(RepositoryError::Unsupported("DeleteSnapshotBlobs"));
    }
};

/// Fake regctl, a POSIX managed-layer root, and a scratch dir.
struct Fixture {
    testing::FakeRegctl fake;
    RefusingRepository repository;
    std::string layer_root;
    std::string scratch;

    Fixture() {
        layer_root = fake.dir() + "/repository";
        scratch = fake.dir() + "/scratch";
        MakeDir(layer_root);
        MakeDir(scratch);
    }

    static void MakeDir(const std::string& path) {
        const std::string command = "mkdir -p '" + path + "'";
        if (::system(command.c_str()) != 0) {
        }
    }

    SnapshotImageService Service() {
        return SnapshotImageService(repository, PosixManagedLayerLocator(layer_root),
                                    Regctl(fake.binary()));
    }

    /// The path `PosixManagedLayerLocator` will look at.
    ///
    /// Built from `BlobKey::PosixPath` rather than spelled out: the layout is
    /// the production one's to decide, and a hand-written copy here would let
    /// a change to it leave these tests seeding files nothing reads.
    std::string PosixLayerPath(const std::string& digest) const {
        return layer_root + "/" + BlobKey::Layer(digest, LayerRole::Rootfs).PosixPath();
    }

    void WriteLayerFile(const std::string& digest, const std::string& contents) const {
        const std::string path = PosixLayerPath(digest);
        MakeDir(path.substr(0, path.rfind('/')));
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    void SeedPosixLayer(const std::string& bytes) const {
        WriteLayerFile(DigestOf(bytes), bytes);
    }

    /// A layer whose on-disk size disagrees with what the record claims.
    void SeedTruncatedPosixLayer(const std::string& bytes) const {
        WriteLayerFile(DigestOf(bytes), "short");
    }
};

CommittedSnapshot CommittedWith(const std::vector<OverlaybdLayerRef>& layers) {
    CommittedSnapshot committed;
    committed.rootfs_layers = layers;
    return committed;
}

RepositoryResult<SnapshotImageResult> Publish(Fixture& f, const CommittedSnapshot& committed) {
    SnapshotImageService service = f.Service();
    return service.PublishCommittedRootfs(SomeId(), committed,
                                          core::Optional<std::string>(std::string(kTargetRepo)),
                                          core::Optional<std::string>(std::string("snap-1")),
                                          f.scratch);
}

// ---- the happy path -------------------------------------------------------

/// One managed layer, end to end: config blob, layer blob and manifest all
/// land, and the returned digest is the one the registry computed.
MT_TEST(image_service_publishes_a_managed_rootfs_layer) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedPosixLayer(bytes);
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, committed);
    MT_EXPECT_TRUE(result.ok());
    MT_EXPECT_TRUE(!result.value().reused);
    MT_EXPECT_EQ(result.value().image_ref, std::string(kTargetRef));
    // The fake hashed what it stored, so this is what a registry would report
    // for the bytes that actually arrived -- not merely what we computed.
    MT_EXPECT_EQ(result.value().manifest_digest, f.fake.StoredManifestDigest(kTargetRef));
    MT_EXPECT_EQ(f.fake.ReadBlob(kTargetRepo, DigestOf(bytes)), bytes);

    // The config blob is named by the manifest and must be there too.
    const core::Expected<core::Json, core::AnyError> manifest =
        core::Json::Parse(f.fake.StoredManifestBody(kTargetRef));
    MT_EXPECT_TRUE(manifest.ok());
    const std::string config_digest =
        manifest.value().as_object().find("config")->second.as_object()
            .find("digest")->second.as_string();
    MT_EXPECT_TRUE(f.fake.BlobExists(kTargetRepo, config_digest));
}

/// The manifest goes last, after every blob it names. Published first it would
/// describe an image that cannot be pulled, and a puller cannot tell that from
/// a corrupt one.
MT_TEST(image_service_puts_the_manifest_after_the_blobs) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedPosixLayer(bytes);
    MT_EXPECT_TRUE(Publish(f, CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)))).ok());

    const std::vector<std::string> argv = f.fake.ArgvLines();
    std::size_t last_put = 0;
    std::size_t manifest_put = 0;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (argv[i].find("argv:blob put") == 0) last_put = i;
        if (argv[i].find("argv:manifest put") == 0) manifest_put = i;
    }
    MT_EXPECT_TRUE(manifest_put > last_put);
}

// ---- transfer shapes ------------------------------------------------------

/// The three shapes, chosen per layer. Asserted on argv because all three end
/// with the blob in the right place: only the cost differs, and a mount that
/// silently became a pipe would move every byte over the network twice.
MT_TEST(image_service_chooses_mount_pipe_and_upload_per_layer) {
    Fixture f;
    const std::string same = "same-registry layer";
    const std::string managed = "managed local layer";
    const std::string cross = "cross-registry layer";
    f.SeedPosixLayer(managed);
    f.fake.SeedBlob("reg.example/team/app", DigestOf(same), same);
    f.fake.SeedBlob("other.example/team/app", DigestOf(cross), cross);

    std::vector<OverlaybdLayerRef> layers;
    layers.push_back(ExternalRef(same, kSrcUrl));
    layers.push_back(ManagedRef(managed));
    layers.push_back(ExternalRef(cross, kCrossUrl));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, CommittedWith(layers));
    MT_EXPECT_TRUE(result.ok());

    // Same registry, different repository: a server-side mount, no bytes move.
    MT_EXPECT_TRUE(f.fake.Logged("blob copy reg.example/team/app " +
                                 std::string(kTargetRepo) + " " + DigestOf(same)));
    // Managed: uploaded from the local store.
    MT_EXPECT_TRUE(f.fake.Logged("blob put " + std::string(kTargetRepo) + " --digest " +
                                 DigestOf(managed)));
    // Different registry: streamed through `blob get | blob put`.
    MT_EXPECT_TRUE(f.fake.Logged("blob get other.example/team/app " + DigestOf(cross)));
    MT_EXPECT_EQ(f.fake.ReadBlob(kTargetRepo, DigestOf(cross)), cross);
}

/// An OSS-managed layer is recorded as *external* with a synthetic `s3://`
/// URL. The marker has to win over the external branch, or `regctl` would be
/// pointed at a host that serves no registry API.
MT_TEST(image_service_treats_s3_external_layers_as_managed) {
    Fixture f;
    const std::string bytes = "oss managed bytes";
    f.SeedPosixLayer(bytes);
    const CommittedSnapshot committed = CommittedWith(std::vector<OverlaybdLayerRef>(
        1, ExternalRef(bytes, "s3://bucket/prefix/managed-layers")));

    MT_EXPECT_TRUE(Publish(f, committed).ok());
    MT_EXPECT_TRUE(f.fake.Logged("blob put " + std::string(kTargetRepo) + " --digest " +
                                 DigestOf(bytes)));
    // No registry fetch was attempted against the s3 URL.
    const std::vector<std::string> argv = f.fake.ArgvLines();
    for (std::size_t i = 0; i < argv.size(); ++i) {
        MT_EXPECT_TRUE(argv[i].find("argv:blob get") != 0);
    }
}

/// A blob the target already has is neither materialised nor transferred. For
/// the managed arm that is the difference between reading a path and
/// downloading gigabytes.
MT_TEST(image_service_skips_layers_the_target_already_has) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    // Deliberately *not* seeded locally: if the service tried to materialise
    // it, the locator would fail and the test would catch it.
    f.fake.SeedBlob(kTargetRepo, DigestOf(bytes), bytes);
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)));

    MT_EXPECT_TRUE(Publish(f, committed).ok());
    MT_EXPECT_TRUE(!f.fake.Logged("blob put " + std::string(kTargetRepo) + " --digest " +
                                  DigestOf(bytes)));
}

/// Same registry *and* repository, yet the head probe proved the blob absent:
/// there is nowhere to transfer it from, so the record points at something the
/// registry no longer holds.
MT_TEST(image_service_reports_an_absent_same_repository_layer) {
    Fixture f;
    const std::string bytes = "vanished layer";
    const CommittedSnapshot committed = CommittedWith(std::vector<OverlaybdLayerRef>(
        1, ExternalRef(bytes, "https://reg.example/v2/team/newapp/blobs")));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, committed);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == RepositoryErrorKind::ArtifactNotFound);
}

// ---- reuse and conflict ---------------------------------------------------

/// Republishing the identical snapshot transfers nothing: the manifest digest
/// is deterministic, so the second attempt recognises its own work.
MT_TEST(image_service_reuses_an_identical_published_image) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedPosixLayer(bytes);
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)));

    const RepositoryResult<SnapshotImageResult> first = Publish(f, committed);
    MT_EXPECT_TRUE(first.ok() && !first.value().reused);
    f.fake.ClearLog();

    const RepositoryResult<SnapshotImageResult> second = Publish(f, committed);
    MT_EXPECT_TRUE(second.ok() && second.value().reused);
    MT_EXPECT_EQ(second.value().manifest_digest, first.value().manifest_digest);
    // Exactly one call: the head probe, and nothing else.
    MT_EXPECT_EQ(f.fake.ArgvLines().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(f.fake.Logged(std::string("manifest head ") + kTargetRef));
}

/// A different manifest at the ref is refused, not overwritten. Overwriting
/// would repoint a tag someone may be pulling and orphan the manifest already
/// there, making the layers it names collectable.
MT_TEST(image_service_refuses_to_overwrite_a_foreign_manifest) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedPosixLayer(bytes);
    f.fake.SeedManifest(kTargetRef, DigestOf("foreign manifest"));
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, committed);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == RepositoryErrorKind::InvalidRequest);
    // Refused *before* anything moved: the head probe alone.
    MT_EXPECT_EQ(f.fake.ArgvLines().size(), static_cast<std::size_t>(1));
}

// ---- managed layer integrity ----------------------------------------------

/// A record naming a layer the store has lost is a managed-layer failure, not
/// a backend fault: retrying cannot bring it back.
MT_TEST(image_service_reports_a_missing_managed_layer) {
    Fixture f;
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef("never seeded")));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, committed);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == RepositoryErrorKind::ManagedLayerNotFound);
}

/// Checked before the upload. The registry would reject the digest anyway, but
/// only after the bytes had been sent and far from the cause.
MT_TEST(image_service_rejects_a_truncated_managed_layer) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedTruncatedPosixLayer(bytes);
    const CommittedSnapshot committed = CommittedWith(
        std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes)));

    const RepositoryResult<SnapshotImageResult> result = Publish(f, committed);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == RepositoryErrorKind::IntegrityMismatch);
    // Nothing was uploaded.
    MT_EXPECT_TRUE(!f.fake.BlobExists(kTargetRepo, DigestOf(bytes)));
}

// ---- input validation -----------------------------------------------------

MT_TEST(image_service_refuses_a_committed_snapshot_without_rootfs_layers) {
    Fixture f;
    const RepositoryResult<SnapshotImageResult> result =
        Publish(f, CommittedWith(std::vector<OverlaybdLayerRef>()));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().kind == RepositoryErrorKind::InvalidRequest);
    // Refused before the registry was touched at all.
    MT_EXPECT_EQ(f.fake.ArgvLines().size(), static_cast<std::size_t>(0));
}

/// An unknown lookup is a 404; a record whose build has not finished is an
/// invalid request. Collapsing the two would answer "does not exist" for a
/// template that is still building, sending the caller after the wrong thing.
MT_TEST(image_service_separates_unknown_from_uncommitted) {
    Fixture f;
    SnapshotImageService service = f.Service();

    const RepositoryResult<SnapshotImageResult> missing = service.ExportRootfsImage(
        "missing", core::Optional<std::string>(), core::Optional<std::string>());
    MT_EXPECT_TRUE(!missing.ok());
    MT_EXPECT_TRUE(missing.error().kind == RepositoryErrorKind::SnapshotNotFound);

    SnapshotRecord uncommitted;
    uncommitted.id = SomeId();
    f.repository.record = core::Optional<SnapshotRecord>(uncommitted);
    const RepositoryResult<SnapshotImageResult> pending = service.ExportRootfsImage(
        uncommitted.id.ToString(), core::Optional<std::string>(),
        core::Optional<std::string>());
    MT_EXPECT_TRUE(!pending.ok());
    MT_EXPECT_TRUE(pending.error().kind == RepositoryErrorKind::InvalidRequest);
}

/// `ExportRootfsImage` owns its scratch directory and removes it however the
/// publish ends -- including on the failure paths, which is where a leak would
/// otherwise accumulate downloaded layers.
MT_TEST(image_service_export_removes_its_scratch_directory) {
    Fixture f;
    const std::string bytes = "managed layer bytes";
    f.SeedPosixLayer(bytes);

    SnapshotRecord record;
    record.id = SomeId();
    record.committed = core::Optional<CommittedSnapshot>(
        CommittedWith(std::vector<OverlaybdLayerRef>(1, ManagedRef(bytes))));
    f.repository.record = core::Optional<SnapshotRecord>(record);

    SnapshotImageService service = f.Service();
    MT_EXPECT_TRUE(service
                       .ExportRootfsImage(record.id.ToString(),
                                          core::Optional<std::string>(std::string(kTargetRepo)),
                                          core::Optional<std::string>(std::string("snap-1")))
                       .ok());

    // Nothing of ours is left behind under TMPDIR.
    const std::string command =
        "test -z \"$(ls -d /tmp/agentenv-snapshot-image-export-* 2>/dev/null)\"";
    MT_EXPECT_EQ(::system(command.c_str()), 0);
}

}  // namespace

MT_MAIN
