// SPDX-License-Identifier: MIT
// Rust: src/snapshot/publish.rs and the `store_snapshot` / `delete_snapshot`
// half of src/snapshot/store.rs.
//
// Driven over the real OSS repository on an HTTP-level stub, like
// test_record_catalog.cc: the publish sequence is a chain of blob writes
// followed by a compare-and-swap commit, and the ordering rules it relies on
// are only real if the CAS underneath them is.
#include "agentenv/snapshot/publish.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/oss/repository.h"
#include "agentenv/snapshot/store.h"
#include "microtest.h"
#include "oss_client_fixture.h"

using namespace agentenv;                       // NOLINT
using namespace agentenv::snapshot;              // NOLINT
using namespace agentenv::snapshot::repository;  // NOLINT
using namespace agentenv::testing::oss_fixture;  // NOLINT

namespace {

const char* kId = "11111111-2222-3333-4444-555555555555";

core::SnapshotId Id(const char* text) {
    const core::Expected<core::SnapshotId, std::string> id = core::SnapshotId::Parse(text);
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

/// A throwaway directory for image configs and capture files.
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/aenv-publish-XXXXXX";
        const char* made = ::mkdtemp(tmpl);
        MT_EXPECT_TRUE(made != NULL);
        path = made == NULL ? std::string() : made;
    }
    ~TempDir() {
        if (path.empty()) return;
        const std::string cmd = "rm -rf '" + path + "'";
        (void)std::system(cmd.c_str());
    }
    std::string At(const std::string& name) const { return core::fs::Join(path, name); }
};

void WriteFile(const std::string& path, const std::string& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    MT_EXPECT_TRUE(file != NULL);
    if (file == NULL) return;
    if (!bytes.empty()) {
        MT_EXPECT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    }
    std::fclose(file);
}

/// An overlaybd image config with one local lower carrying a descriptor.
std::string LocalLowerConfig(const std::string& file, const std::string& digest,
                             uint64_t size) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(size));
    return "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + file + "\",\"digest\":\"" +
           digest + "\",\"size\":" + buf +
           "}],\"upper\":{},\"resultFile\":\"\",\"download\":{}}";
}

/// An overlaybd image config with one remote lower.
std::string RemoteLowerConfig(const std::string& repo_blob_url, const std::string& digest,
                              uint64_t size) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(size));
    return "{\"repoBlobUrl\":\"\",\"lowers\":[{\"digest\":\"" + digest +
           "\",\"size\":" + buf + ",\"repoBlobUrl\":\"" + repo_blob_url +
           "\"}],\"upper\":{},\"resultFile\":\"\",\"download\":{}}";
}

/// Puts a file-bodied blob into the stub's object map.
///
/// Needed because a layer upload goes through the multipart uploader rather
/// than the HTTP transport, and a null uploader reports `Unsupported`. Writing
/// the bytes in means the blob-existence assertions below are real: a rollback
/// that failed to delete a layer would still be visible here.
struct StubUploader : public oss::ObjectUploader {
    StubObjectServer* server;
    explicit StubUploader(StubObjectServer* s) : server(s) {}

    RepositoryResult<core::Unit> UploadFile(const std::string& object_url,
                                            const std::string& path) {
        // `s3://<bucket>/<key>` -> the key the stub files everything under.
        const std::string marker = "/agentenv/";
        const std::string::size_type at = object_url.find(marker);
        MT_EXPECT_TRUE(at != std::string::npos);
        if (at == std::string::npos) {
            return core::make_unexpected(
                RepositoryError::Backend("stub upload", "unexpected object url " + object_url));
        }
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == NULL) {
            return core::make_unexpected(RepositoryError::Backend("stub upload", path));
        }
        std::string bytes;
        char buf[4096];
        std::size_t read = 0;
        while ((read = std::fread(buf, 1, sizeof(buf), file)) > 0) bytes.append(buf, read);
        std::fclose(file);
        server->Seed(object_url.substr(at + marker.size()), bytes);
        return core::Unit();
    }
};

struct Fixture {
    StubObjectServer server;
    std::shared_ptr<oss::OssSnapshotRepository> repository;
    TempDir dir;

    Fixture() {
        const RepositoryResult<oss::OssClient*> client =
            oss::OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
        MT_EXPECT_TRUE(client.ok());
        repository.reset(new oss::OssSnapshotRepository(
            std::shared_ptr<oss::OssClient>(client.ok() ? client.value() : 0),
            std::shared_ptr<oss::ObjectUploader>(new StubUploader(&server))));
    }

    SnapshotRepository& Repo() { return *repository; }

    /// A manifest whose rootfs, memory and VM state all exist on disk.
    sandbox::SandboxSnapshotManifest Manifest(
        const std::vector<sandbox::ExtraDrive>& drives = std::vector<sandbox::ExtraDrive>()) {
        const std::string rootfs_lower = dir.At("rootfs.commit");
        WriteFile(rootfs_lower, "rootfs-bytes");
        const std::string rootfs_config = dir.At("rootfs-image.json");
        WriteFile(rootfs_config, LocalLowerConfig(rootfs_lower, "sha256:rootfs", 12));

        const std::string memory_lower = dir.At("memory.commit");
        WriteFile(memory_lower, "memory-bytes");
        const std::string memory_config = dir.At("mem-image.json");
        WriteFile(memory_config, LocalLowerConfig(memory_lower, "sha256:memory", 12));

        const std::string vm_state = dir.At("vm_state.bin");
        WriteFile(vm_state, "vm-state");

        const core::Expected<sandbox::SandboxSnapshotManifest, std::string> manifest =
            sandbox::SandboxSnapshotManifest::New(sandbox::kFirecrackerBackend, vm_state,
                                                  memory_config, 1 << 20, rootfs_config,
                                                  1 << 20, drives);
        MT_EXPECT_TRUE(manifest.ok());
        return manifest.ok() ? manifest.value() : sandbox::SandboxSnapshotManifest();
    }

    SnapshotPublishMetadata Metadata() {
        SnapshotPublishMetadata metadata;
        metadata.id = Id(kId);
        metadata.source = SnapshotPublishSource::Sandbox("sbx-1");
        metadata.context.workdir = "/work";
        return metadata;
    }

    bool HasBlob(SnapshotFile file) {
        return server.bodies.count(BlobKey::Snapshot(Id(kId), file).Path()) != 0;
    }

    core::Optional<SnapshotRecord> Stored() {
        const std::string key = DocumentKey::SnapshotRecord(Id(kId)).Path();
        if (server.bodies.find(key) == server.bodies.end()) {
            return core::Optional<SnapshotRecord>();
        }
        const core::Expected<core::Json, core::AnyError> json =
            core::Json::Parse(server.bodies[key]);
        MT_EXPECT_TRUE(json.ok());
        const core::Expected<SnapshotRecord, std::string> record =
            SnapshotRecord::FromJson(json.value());
        return record.ok() ? core::Optional<SnapshotRecord>(record.value())
                           : core::Optional<SnapshotRecord>();
    }
};

sandbox::ExtraDrive Drive(const std::string& id, uint64_t virtual_size,
                          const std::string& image_config_path) {
    sandbox::ExtraDrive drive;
    drive.drive_id = id;
    drive.virtual_size = core::Optional<uint64_t>(virtual_size);
    drive.image_config_path = image_config_path;
    drive.mount_path = "/mnt/" + id;
    return drive;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- the happy path -------------------------------------------------------

MT_TEST(publish_stores_the_artifacts_then_commits_the_record) {
    Fixture f;
    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), f.Manifest());
    MT_EXPECT_TRUE(published.ok());

    // The VM state and the manifest are both stored ...
    MT_EXPECT_TRUE(f.HasBlob(SnapshotFile::VmState));
    MT_EXPECT_TRUE(f.HasBlob(SnapshotFile::FirecrackerManifest));
    // ... and the record names the layers rather than local paths, which is
    // what makes it readable on another node.
    MT_EXPECT_TRUE(published.ok() && published.value().record.committed.has_value());
    MT_EXPECT_TRUE(published.ok() &&
                   published.value().record.committed->rootfs_layers.size() == 1);
    MT_EXPECT_TRUE(published.ok() &&
                   published.value().record.committed->memory_layers.size() == 1);
    // The startup pack is attached by a later continuation, so it is absent
    // here -- as it is upstream when publish returns.
    MT_EXPECT_TRUE(published.ok() &&
                   !published.value().record.committed->memory_startup.has_value());
}

MT_TEST(the_stored_manifest_is_pretty_printed_like_the_rust_server_writes_it) {
    Fixture f;
    MT_EXPECT_TRUE(Publish(f.Repo(), f.Metadata(), f.Manifest()).ok());
    const std::string body =
        f.server.bodies[BlobKey::Snapshot(Id(kId), SnapshotFile::FirecrackerManifest).Path()];
    // `to_vec_pretty`: a restore on a Rust node parses either form, but the
    // bytes are what a cross-implementation digest would be taken over.
    MT_EXPECT_TRUE(Contains(body, "\n"));
    MT_EXPECT_TRUE(Contains(body, "\"backend\""));
}

// ---- validate_attached_drives --------------------------------------------

MT_TEST(duplicate_attached_drive_ids_are_refused_before_anything_is_uploaded) {
    Fixture f;
    // Attached directly for the same reason as the zero-size case below: the
    // manifest constructor rejects a duplicate id, so reaching publish's own
    // check needs a manifest that did not come through it.
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, "{\"repoBlobUrl\":\"\",\"lowers\":[],\"upper\":{},\"resultFile\":\"\"}");
    sandbox::SnapshotAttachedDriveArtifacts drive;
    drive.drive_id = "data";
    drive.virtual_size = 4096;
    drive.image_config_path = config;
    drive.mount_path = "/mnt/data";
    manifest.attached_drives.push_back(drive);
    manifest.attached_drives.push_back(drive);

    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), manifest);
    MT_EXPECT_TRUE(!published.ok());
    MT_EXPECT_TRUE(!published.ok() &&
                   Contains(published.error().ToString(), "duplicate attached drive id"));
    // Two drives with one id would publish a disk the restore cannot address:
    // the record keys them by id, so one captured disk would silently vanish.
    // Checked before any upload, so nothing was written.
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
    MT_EXPECT_TRUE(!f.Stored().has_value());
}

MT_TEST(a_zero_virtual_size_drive_is_refused) {
    Fixture f;
    // `SandboxSnapshotManifest::New` already rejects a zero size, so the drive
    // is attached directly to a built manifest. That is the point of this
    // test: publish's check is the second line of defence, covering a manifest
    // that reached it without going through that constructor -- one decoded
    // from a stored document, say. Routing this through `New` would assert the
    // constructor's rule twice and leave publish's unexercised.
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, "{\"repoBlobUrl\":\"\",\"lowers\":[],\"upper\":{},\"resultFile\":\"\"}");
    sandbox::SnapshotAttachedDriveArtifacts drive;
    drive.drive_id = "data";
    drive.virtual_size = 0;
    drive.image_config_path = config;
    drive.mount_path = "/mnt/data";
    manifest.attached_drives.push_back(drive);

    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), manifest);
    MT_EXPECT_TRUE(!published.ok());
    // A zero size cannot be mounted on restore, and the failure would surface
    // there -- long after the publish reported success.
    MT_EXPECT_TRUE(!published.ok() &&
                   Contains(published.error().ToString(), "virtual_size must be non-zero"));
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
}

// ---- ensure_not_committed -------------------------------------------------

MT_TEST(republishing_a_committed_snapshot_is_refused_without_touching_its_files) {
    Fixture f;
    MT_EXPECT_TRUE(Publish(f.Repo(), f.Metadata(), f.Manifest()).ok());
    const std::string manifest_key =
        BlobKey::Snapshot(Id(kId), SnapshotFile::FirecrackerManifest).Path();
    const std::string first = f.server.bodies[manifest_key];

    const RepositoryResult<SnapshotPublication> again =
        Publish(f.Repo(), f.Metadata(), f.Manifest());
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(!again.ok() && Contains(again.error().ToString(), "already committed"));

    // The live snapshot's files are intact. Without this gate the second
    // publish would overwrite them and then, when its commit failed, delete
    // them -- taking the live snapshot's artifacts with it.
    MT_EXPECT_TRUE(f.server.bodies[manifest_key] == first);
    MT_EXPECT_TRUE(f.Stored().has_value() && f.Stored()->committed.has_value());
}

MT_TEST(an_uncommitted_record_does_not_block_publishing) {
    Fixture f;
    // A pre-created template build: `committed` is absent, so the publish that
    // completes it must be allowed through.
    SnapshotRecord waiting;
    waiting.id = Id(kId);
    waiting.source = SnapshotSource::Template(TemplateBuildInfo());
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kId)).Path(),
                  waiting.ToJson().ToStringPretty());

    MT_EXPECT_TRUE(Publish(f.Repo(), f.Metadata(), f.Manifest()).ok());
}

MT_TEST(an_unparsable_record_blocks_the_publish_rather_than_being_ignored) {
    Fixture f;
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kId)).Path(), "{not json");
    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), f.Manifest());
    MT_EXPECT_TRUE(!published.ok());
    // Treating an unreadable record as "not committed" would overwrite a live
    // snapshot's files, so this must fail closed.
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
}

MT_TEST(the_committed_gate_reads_untyped_json_so_an_unknown_field_still_blocks) {
    Fixture f;
    // A record from a newer server: `committed` is present but carries a shape
    // this build's typed decoder would reject. The gate must still see it,
    // which is why it inspects untyped JSON.
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kId)).Path(),
                  "{\"id\":\"" + std::string(kId) +
                      "\",\"committed\":{\"from_the_future\":true}}");
    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), f.Manifest());
    MT_EXPECT_TRUE(!published.ok());
    MT_EXPECT_TRUE(!published.ok() &&
                   Contains(published.error().ToString(), "already committed"));
}

MT_TEST(a_null_committed_field_is_not_committed) {
    Fixture f;
    // `is_some_and(|c| !c.is_null())`: serde writes an absent `committed` as a
    // missing key, but a hand-written or migrated document may carry an
    // explicit null. Treating null as committed would permanently refuse to
    // publish such a snapshot.
    //
    // Built from a real record and then given an explicit null, rather than
    // hand-rolled: the gate must let it through *and* the commit that follows
    // has to decode it, so a stub document missing `source` would fail for an
    // unrelated reason and prove nothing about null handling.
    SnapshotRecord waiting;
    waiting.id = Id(kId);
    waiting.source = SnapshotSource::Template(TemplateBuildInfo());
    core::JsonObject fields = waiting.ToJson().as_object();
    fields["committed"] = core::Json();
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kId)).Path(),
                  core::Json(fields).ToStringPretty());

    MT_EXPECT_TRUE(Publish(f.Repo(), f.Metadata(), f.Manifest()).ok());
}

// ---- rollback -------------------------------------------------------------

MT_TEST(a_failed_commit_removes_the_files_the_publish_stored) {
    Fixture f;
    SnapshotPublishMetadata metadata = f.Metadata();
    const core::Expected<SnapshotAlias, std::string> alias = SnapshotAlias::Parse("base");
    MT_EXPECT_TRUE(alias.ok());
    metadata.alias = core::Optional<SnapshotAlias>(alias.value());

    // A competing snapshot takes the alias *after* the early availability
    // check has passed and before the commit claims it -- the hook fires on
    // the first blob PUT, which is already inside the store step. That is the
    // real race the commit's atomic claim exists to lose safely.
    const char* kOther = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    f.server.before_put = [&f, kOther]() {
        SnapshotRecord owner;
        owner.id = Id(kOther);
        owner.source = SnapshotSource::Template(TemplateBuildInfo());
        f.server.Seed(DocumentKey::SnapshotRecord(Id(kOther)).Path(),
                      owner.ToJson().ToStringPretty());
        f.server.Seed(DocumentKey::SnapshotAlias(SnapshotAlias::Parse("base").value()).Path(),
                      core::Json(std::string(kOther)).ToString());
    };

    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), metadata, f.Manifest());
    MT_EXPECT_TRUE(!published.ok());

    // The blobs this publish wrote are gone: left behind, they would be
    // referenced by no record and collected by nothing.
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::FirecrackerManifest));
    // And the record was rolled back with them.
    MT_EXPECT_TRUE(!f.Stored().has_value());
}

MT_TEST(a_taken_alias_is_refused_before_anything_is_uploaded) {
    Fixture f;
    const char* kOther = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    SnapshotRecord owner;
    owner.id = Id(kOther);
    owner.source = SnapshotSource::Template(TemplateBuildInfo());
    f.server.Seed(DocumentKey::SnapshotRecord(Id(kOther)).Path(),
                  owner.ToJson().ToStringPretty());
    f.server.Seed(DocumentKey::SnapshotAlias(SnapshotAlias::Parse("base").value()).Path(),
                  core::Json(std::string(kOther)).ToString());

    SnapshotPublishMetadata metadata = f.Metadata();
    metadata.alias = core::Optional<SnapshotAlias>(SnapshotAlias::Parse("base").value());

    MT_EXPECT_TRUE(!Publish(f.Repo(), metadata, f.Manifest()).ok());
    // The early check is what keeps a doomed publish from uploading a whole
    // memory image first.
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
}

// ---- store_memory_layers --------------------------------------------------

MT_TEST(a_remote_memory_lower_is_refused_when_it_is_not_this_repositorys) {
    Fixture f;
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    // A memory lower with no local file is named by digest alone in the
    // record, so a digest living in someone else's registry would resolve to
    // nothing on restore.
    const std::string config = f.dir.At("mem-remote.json");
    WriteFile(config,
              RemoteLowerConfig("https://registry.example/v2/ns/image/blobs", "sha256:m", 4096));
    manifest.memory.image_config_path = config;

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest);
    MT_EXPECT_TRUE(!stored.ok());
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "memory layer 0"));
}

MT_TEST(a_local_memory_lower_without_a_descriptor_is_hashed_rather_than_refused) {
    Fixture f;
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    // Unlike a disk lower, a freshly captured memory delta carries no
    // descriptor -- refusing it would make every memory capture unpublishable.
    const std::string lower = f.dir.At("mem-nodesc.commit");
    WriteFile(lower, "memory-bytes");
    const std::string config = f.dir.At("mem-nodesc.json");
    WriteFile(config, "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + lower +
                          "\"}],\"upper\":{},\"resultFile\":\"\",\"download\":{}}");
    manifest.memory.image_config_path = config;

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest);
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(stored.ok() && stored.value().memory_layers.size() == 1);
    // Hashed, so the digest is real content addressing rather than a label.
    MT_EXPECT_TRUE(stored.ok() &&
                   Contains(stored.value().memory_layers[0].digest, "sha256:"));
}

// ---- validate_image_configs ----------------------------------------------

MT_TEST(a_malformed_image_config_fails_preflight_before_any_upload) {
    Fixture f;
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    const std::string config = f.dir.At("mem-broken.json");
    WriteFile(config, "{not json");
    manifest.memory.image_config_path = config;

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest);
    MT_EXPECT_TRUE(!stored.ok());
    MT_EXPECT_TRUE(!stored.ok() &&
                   Contains(stored.error().ToString(), "validate memory image config"));
    // The whole point of the separate pass: the rootfs layers were not stored
    // first and then abandoned.
    MT_EXPECT_TRUE(!f.HasBlob(SnapshotFile::VmState));
}

MT_TEST(a_malformed_drive_config_names_the_drive_it_belongs_to) {
    Fixture f;
    const std::string broken = f.dir.At("drive-broken.json");
    WriteFile(broken, "{not json");
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(Drive("data", 4096, broken));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(drives));
    MT_EXPECT_TRUE(!stored.ok());
    // An operator needs to know *which* drive, since a sandbox can have many.
    MT_EXPECT_TRUE(!stored.ok() &&
                   Contains(stored.error().ToString(), "for drive 'data'"));
}

// ---- attached drives -----------------------------------------------------

MT_TEST(an_attached_drives_mount_path_falls_back_to_the_default_when_unusable) {
    Fixture f;
    const std::string lower = f.dir.At("drive.commit");
    WriteFile(lower, "drive-bytes");
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, LocalLowerConfig(lower, "sha256:drive", 11));

    sandbox::ExtraDrive drive = Drive("data", 4096, config);
    // A relative path cannot be normalised. The capture already happened and
    // the path is reconstructible at restore, so this must not fail the
    // publish.
    drive.mount_path = "relative/path";
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(drive);

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(drives));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(stored.ok() && stored.value().attached_drives.size() == 1);
    MT_EXPECT_TRUE(stored.ok() &&
                   stored.value().attached_drives[0].mount_path ==
                       sandbox::ExtraDrive::DefaultMountPath("data"));
}

MT_MAIN
