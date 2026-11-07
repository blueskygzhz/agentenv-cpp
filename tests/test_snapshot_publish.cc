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

    /// `image_storage` defaults to the repository's own default
    /// (`ObjectStorage`), so a test that is not about the source-registry arm
    /// gets managed layers without saying so.
    explicit Fixture(cfg::SnapshotImageStoragePolicy image_storage =
                         cfg::SnapshotImageStoragePolicy::ObjectStorage) {
        const RepositoryResult<oss::OssClient*> client =
            oss::OssClient::NewWithTransport(Normalized(GoodConfig()), server.Fn());
        MT_EXPECT_TRUE(client.ok());
        PublishPolicy policy;
        policy.image_storage = image_storage;
        repository.reset(new oss::OssSnapshotRepository(
            std::shared_ptr<oss::OssClient>(client.ok() ? client.value() : 0),
            std::shared_ptr<oss::ObjectUploader>(new StubUploader(&server)), policy));
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

/// The managed-layer location the OSS fixture reports, which is what a remote
/// lower has to match for the fallback to be allowed.
const char* kManagedLayers = "s3://agentenv/managed-layers";

/// One recorded call to the injected exporter.
struct ExportCall {
    image_export::DiskImageSubject subject;
    std::string image_config_path;
    bool had_config;
};

/// A `SourceRegistryExporter` that records what it was asked to publish and
/// answers from a script, plus the rollbacks it was later asked for.
///
/// Shared through a `shared_ptr` because the functors outlive the expression
/// that builds them and both halves must see the same recording.
struct FakeExporter {
    std::vector<ExportCall> calls;
    std::vector<PersistedDiskImagePublication> rollbacks;
    /// Returned in order, one per call; the last repeats once exhausted.
    std::vector<RepositoryResult<image_export::DiskImageExportOutcome> > script;
    bool rollback_fails = false;
};

/// A successful outcome naming one external layer plus a publication.
RepositoryResult<image_export::DiskImageExportOutcome> Published(const std::string& tag) {
    image_export::DiskImageExportOutcome outcome;
    ExternalLayer layer;
    layer.digest = "sha256:published-" + tag;
    layer.repo_blob_url = "https://registry.example.com/v2/app/blobs";
    layer.size = 7;
    outcome.layers.push_back(OverlaybdLayerRef::External(layer));
    PersistedDiskImagePublication publication;
    publication.image_ref = "registry.example.com/app:" + tag;
    publication.tag = tag;
    publication.manifest_digest = "sha256:manifest-" + tag;
    publication.repo_blob_url = "https://registry.example.com/v2/app/blobs";
    outcome.publication = core::Optional<PersistedDiskImagePublication>(publication);
    return outcome;
}

SourceRegistryExporter Exporter(const std::shared_ptr<FakeExporter>& fake) {
    SourceRegistryExporter out;
    out.publish = [fake](const core::SnapshotId&, const image_export::DiskImageSubject& subject,
                         const std::string& image_config_path,
                         const core::Optional<SnapshotOciConfigInput>& config) {
        ExportCall call = {subject, image_config_path, config.has_value()};
        fake->calls.push_back(call);
        const std::size_t index = fake->calls.size() - 1;
        MT_EXPECT_TRUE(!fake->script.empty());
        if (fake->script.empty()) {
            return RepositoryResult<image_export::DiskImageExportOutcome>(
                core::make_unexpected(RepositoryError::Backend("no script")));
        }
        return fake->script[index < fake->script.size() ? index : fake->script.size() - 1];
    };
    out.rollback = [fake](const PersistedDiskImagePublication& publication) {
        fake->rollbacks.push_back(publication);
        if (fake->rollback_fails) {
            return RepositoryResult<core::Unit>(
                core::make_unexpected(RepositoryError::Backend("rollback refused")));
        }
        return RepositoryResult<core::Unit>(core::Unit());
    };
    return out;
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

// ---- store_disk_image: the exporter selection ----------------------------

/// The policy decides, not the availability of an exporter.
///
/// This is the property the whole selection rests on, and the one that would
/// break silently: a repository that stores managed layers must never reach
/// the registry, even when a perfectly good exporter is in hand. Asserted by
/// handing one in and requiring that it was *not* called.
MT_TEST(object_storage_policy_never_reaches_the_exporter) {
    Fixture f(cfg::SnapshotImageStoragePolicy::ObjectStorage);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(fake->calls.empty());
    // Managed layers instead, so the bytes landed here.
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers.size() == 1);
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers[0].is_managed());
    MT_EXPECT_TRUE(stored.ok() && stored.value().disk_publications.empty());
}

/// The source-registry policy publishes, and the publication reaches the
/// caller rather than being dropped on the floor.
MT_TEST(source_registry_policy_publishes_and_returns_the_publication) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(fake->calls.size() == 1);
    // The layers are the exporter's, not locally stored ones.
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers.size() == 1);
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers[0].is_external());
    MT_EXPECT_TRUE(stored.ok() && stored.value().disk_publications.size() == 1);
    MT_EXPECT_TRUE(stored.ok() &&
                   stored.value().disk_publications[0].tag == "rootfs");
}

/// The rootfs publishes *with* a config and a drive publishes *without* one,
/// and a drive's subject carries its id.
///
/// Both halves matter. A drive that published the rootfs' effective runtime
/// config would describe the wrong image, and a subject that lost the drive id
/// would make every drive of one snapshot publish under the rootfs tag --
/// overwriting each other.
MT_TEST(the_subject_and_config_differ_between_the_rootfs_and_a_drive) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));
    fake->script.push_back(Published("drive"));

    const std::string lower = f.dir.At("drive.commit");
    WriteFile(lower, "drive-bytes");
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, LocalLowerConfig(lower, "sha256:drive", 11));
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(Drive("data", 4096, config));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(drives), Exporter(fake));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(fake->calls.size() == 2);
    if (fake->calls.size() != 2) return;

    MT_EXPECT_TRUE(fake->calls[0].subject.is_rootfs());
    MT_EXPECT_TRUE(fake->calls[0].had_config);
    MT_EXPECT_TRUE(!fake->calls[1].subject.is_rootfs());
    MT_EXPECT_TRUE(fake->calls[1].subject.drive_id == "data");
    MT_EXPECT_TRUE(!fake->calls[1].had_config);
    // Each publishes its own image config, not the other's.
    MT_EXPECT_TRUE(fake->calls[1].image_config_path == config);
    MT_EXPECT_TRUE(fake->calls[0].image_config_path != config);

    // Rootfs first, then the drive: `DeleteSnapshot` unwinds this in reverse.
    MT_EXPECT_TRUE(stored.value().disk_publications.size() == 2);
    MT_EXPECT_TRUE(stored.value().disk_publications[0].tag == "rootfs");
    MT_EXPECT_TRUE(stored.value().disk_publications[1].tag == "drive");
}

// ---- store_disk_image: the fallback --------------------------------------

/// `Unsupported` falls back to managed layers when nothing would be mixed.
MT_TEST(an_unsupported_image_shape_falls_back_to_managed_layers) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(core::make_unexpected(
        RepositoryError::Unsupported("descriptorless lower")));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(fake->calls.size() == 1);
    // Stored here instead, and nothing was published.
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers.size() == 1);
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers[0].is_managed());
    MT_EXPECT_TRUE(stored.ok() && stored.value().disk_publications.empty());
}

/// Only `Unsupported` falls back. Every other error propagates.
///
/// The distinction is the point: `Unsupported` is a statement about the
/// image's shape, which storing it locally answers. A refused auth or a failed
/// upload is a statement about *this attempt*, and falling back would turn a
/// misconfigured registry into a silent no-op -- the operator asked to publish
/// and would be told it worked.
MT_TEST(a_transport_error_does_not_fall_back) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Backend("push layer", "401 unauthorized")));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(!stored.ok());
    MT_EXPECT_TRUE(!stored.ok() && stored.error().kind == RepositoryErrorKind::Backend);
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "401 unauthorized"));
}

/// A remote lower pointing outside this repository refuses the fallback.
///
/// Storing the local lowers here while leaving that one where it is would
/// leave the image half in this bucket and half in a registry nothing here
/// manages: deleting the snapshot would orphan the other half, and a restore
/// would depend on a source no one on this node can keep alive.
MT_TEST(a_foreign_remote_lower_refuses_the_fallback) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Unsupported("unmodelled layer")));

    // The rootfs image config's lower lives in someone else's registry.
    const std::string rootfs_config = f.dir.At("rootfs-image.json");
    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    WriteFile(rootfs_config,
              RemoteLowerConfig("https://elsewhere.example.com/v2/app/blobs",
                                "sha256:foreign", 9));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest, Exporter(fake));
    MT_EXPECT_TRUE(!stored.ok());
    MT_EXPECT_TRUE(!stored.ok() && stored.error().kind == RepositoryErrorKind::Unsupported);
    // The original reason is kept, wrapped rather than replaced: without it
    // the operator cannot tell which layer shape was refused.
    MT_EXPECT_TRUE(!stored.ok() &&
                   Contains(stored.error().ToString(), "remote-backed disk image"));
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "unmodelled layer"));
}

/// A remote lower that already lives in *this* repository allows the fallback.
///
/// This is the other side of the test above, and it is what keeps the guard
/// from refusing every remote lower outright: a lower already in this bucket
/// mixes nothing, so storing the rest here yields one coherent image.
MT_TEST(a_remote_lower_in_this_repository_allows_the_fallback) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Unsupported("unmodelled layer")));

    sandbox::SandboxSnapshotManifest manifest = f.Manifest();
    WriteFile(f.dir.At("rootfs-image.json"),
              RemoteLowerConfig(kManagedLayers, "sha256:ours", 9));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest, Exporter(fake));
    MT_EXPECT_TRUE(stored.ok());
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers.size() == 1);
    // Left as the external reference it already was.
    MT_EXPECT_TRUE(stored.ok() && stored.value().rootfs_layers[0].is_external());
    MT_EXPECT_TRUE(stored.ok() && stored.value().disk_publications.empty());
}

// ---- the cleanup rolls publications back ---------------------------------

/// A publish that dies partway rolls back what it already published.
///
/// Rust keeps its `LayerStore` alive past the `match` so the error arm can
/// reach `store.disk_publications`; this port threads a partial result out for
/// the same reason. Without it the rootfs image stays in the registry with no
/// record naming it, which is the one leak nothing later can find.
MT_TEST(a_failure_after_publishing_rolls_the_publication_back) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));
    // The drive's export fails in a way that does not fall back.
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Backend("push drive", "503")));

    const std::string lower = f.dir.At("drive.commit");
    WriteFile(lower, "drive-bytes");
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, LocalLowerConfig(lower, "sha256:drive", 11));
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(Drive("data", 4096, config));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(drives), Exporter(fake));
    MT_EXPECT_TRUE(!stored.ok());
    // The rootfs publication was undone, and it is the one that was made.
    MT_EXPECT_TRUE(fake->rollbacks.size() == 1);
    MT_EXPECT_TRUE(fake->rollbacks.size() == 1 && fake->rollbacks[0].tag == "rootfs");
    // The caller still sees the original cause, not a rollback detail.
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "503"));
}

/// A rollback that itself fails is logged and stepped over.
///
/// The store error is what the caller must see: it is the actionable cause,
/// and a registry that is down must not also mask it.
MT_TEST(a_failing_rollback_does_not_replace_the_store_error) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->rollback_fails = true;
    fake->script.push_back(Published("rootfs"));
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Backend("push drive", "503")));

    const std::string lower = f.dir.At("drive.commit");
    WriteFile(lower, "drive-bytes");
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, LocalLowerConfig(lower, "sha256:drive", 11));
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(Drive("data", 4096, config));

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), f.Manifest(drives), Exporter(fake));
    MT_EXPECT_TRUE(!stored.ok());
    MT_EXPECT_TRUE(fake->rollbacks.size() == 1);
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "503"));
    MT_EXPECT_TRUE(!stored.ok() &&
                   !Contains(stored.error().ToString(), "rollback refused"));
}

/// A cleanup that itself fails is logged, never returned.
///
/// The store error is the actionable cause and the only one the caller can do
/// anything about. Returning the cleanup error instead would report "could not
/// delete blobs" for a publish that actually failed because the registry
/// refused it, pointing every investigation at the wrong subsystem.
MT_TEST(a_failing_blob_cleanup_does_not_replace_the_store_error) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    // The rootfs succeeds and the drive fails, so the store gets far enough to
    // have written blobs -- without that the cleanup has nothing to delete,
    // never issues a DELETE, and cannot fail at all.
    fake->script.push_back(Published("rootfs"));
    fake->script.push_back(
        core::make_unexpected(RepositoryError::Backend("push drive", "registry refused")));

    const std::string lower = f.dir.At("drive.commit");
    WriteFile(lower, "drive-bytes");
    const std::string config = f.dir.At("drive-image.json");
    WriteFile(config, LocalLowerConfig(lower, "sha256:drive", 11));
    std::vector<sandbox::ExtraDrive> drives;
    drives.push_back(Drive("data", 4096, config));
    const sandbox::SandboxSnapshotManifest manifest = f.Manifest(drives);

    // Built after the manifest, so the fixture's own writes still land.
    f.server.fail_delete = true;

    const RepositoryResult<StoredSnapshot> stored =
        StoreSnapshot(f.Repo(), f.Metadata(), manifest, Exporter(fake));
    MT_EXPECT_TRUE(!stored.ok());
    // The store's own cause survives ...
    MT_EXPECT_TRUE(!stored.ok() && Contains(stored.error().ToString(), "registry refused"));
    // ... and the cleanup's failure did happen, but was not what came back.
    MT_EXPECT_TRUE(!stored.ok() && !Contains(stored.error().ToString(), "500"));
    // The blobs are still there, which is what made the cleanup fail.
    MT_EXPECT_TRUE(f.HasBlob(SnapshotFile::VmState));
}

/// A commit that fails rolls the publications back too.
///
/// This is the one leak that nothing later could repair: the record is what
/// names a publication, so a commit that never lands leaves the image in the
/// registry with no way to find it again. Upstream passes
/// `&disk_publications` to its cleanup for exactly this.
MT_TEST(a_failed_commit_rolls_the_publications_back) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));

    SnapshotPublishMetadata metadata = f.Metadata();
    const core::Expected<SnapshotAlias, std::string> alias = SnapshotAlias::Parse("base");
    MT_EXPECT_TRUE(alias.ok());
    metadata.alias = core::Optional<SnapshotAlias>(alias.value());

    // The same race as above: a competing snapshot claims the alias after the
    // early check and before the commit, so the store step succeeds -- and
    // publishes -- and only the commit fails.
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
        Publish(f.Repo(), metadata, f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(!published.ok());
    // The publish happened, so there was something to undo ...
    MT_EXPECT_TRUE(fake->calls.size() == 1);
    // ... and it was undone.
    MT_EXPECT_TRUE(fake->rollbacks.size() == 1);
    MT_EXPECT_TRUE(fake->rollbacks.size() == 1 && fake->rollbacks[0].tag == "rootfs");
}

/// `Publish` carries the publications into the committed record.
///
/// The record is the only thing that names them, so a publication that does
/// not reach it can never be rolled back -- the delete path reads exactly this
/// field.
MT_TEST(publish_commits_the_publications_into_the_record) {
    Fixture f(cfg::SnapshotImageStoragePolicy::SourceRegistry);
    const std::shared_ptr<FakeExporter> fake(new FakeExporter());
    fake->script.push_back(Published("rootfs"));

    const RepositoryResult<SnapshotPublication> published =
        Publish(f.Repo(), f.Metadata(), f.Manifest(), Exporter(fake));
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_TRUE(published.ok() && published.value().record.committed.has_value() &&
                   published.value().record.committed->disk_publications.size() == 1);

    // Read back through the record's own serialisation, which is what a
    // delete on another node would see.
    const core::Optional<SnapshotRecord> reread = f.Stored();
    MT_EXPECT_TRUE(reread.has_value());
    MT_EXPECT_TRUE(reread.has_value() && reread->committed.has_value());
    MT_EXPECT_TRUE(reread.has_value() && reread->committed.has_value() &&
                   reread->committed->disk_publications.size() == 1);
    MT_EXPECT_TRUE(reread.has_value() && reread->committed.has_value() &&
                   reread->committed->disk_publications.size() == 1 &&
                   reread->committed->disk_publications[0].manifest_digest ==
                       "sha256:manifest-rootfs");
}

MT_MAIN
