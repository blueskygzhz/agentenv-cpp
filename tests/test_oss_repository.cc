// SPDX-License-Identifier: MIT
// Rust: src/repository/backends/oss/repository.rs
//
// The assembled repository. Everything that matters here is a routing
// decision -- which operation becomes which request, and which answers are
// absence rather than failure -- so it is all observable through the injected
// transport, and the two tests that need a filesystem use a temp directory
// rather than a bucket.
#include "agentenv/snapshot/repository/oss/repository.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/backends.h"
#include "microtest.h"
#include "no_docker_credentials.h"
#include "oss_client_fixture.h"

using namespace agentenv;                              // NOLINT
using namespace agentenv::snapshot;                     // NOLINT
using namespace agentenv::snapshot::repository;         // NOLINT
using namespace agentenv::snapshot::repository::oss;    // NOLINT
using namespace agentenv::testing::oss_fixture;         // NOLINT

namespace {

/// Records what it was asked to upload, which is the only thing the
/// repository decides -- the multipart protocol itself belongs to the
/// overlaybd backend behind this interface.
struct RecordingUploader : public ObjectUploader {
    std::vector<std::pair<std::string, std::string> > uploads;

    RepositoryResult<core::Unit> UploadFile(const std::string& object_url,
                                            const std::string& path) {
        uploads.push_back(std::make_pair(object_url, path));
        return core::Unit();
    }
};

std::shared_ptr<OssClient> ClientOf(const OssClient::Transport& transport,
                                    const cfg::OssBackendConfig& config) {
    const RepositoryResult<OssClient*> client =
        OssClient::NewWithTransport(Normalized(config), transport);
    MT_EXPECT_TRUE(client.ok());
    return std::shared_ptr<OssClient>(client.ok() ? client.value() : 0);
}

core::SnapshotId TestId() {
    const core::Expected<core::SnapshotId, std::string> id =
        core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());
    return id.ok() ? id.value() : core::SnapshotId();
}

struct TempRoot {
    std::string path;
    TempRoot() {
        const core::Expected<std::string, std::string> dir =
            core::fs::CreateTempDir("agentenv-oss-repo-");
        path = dir.ok() ? dir.value() : std::string("/tmp/agentenv-oss-repo-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
    std::string At(const std::string& leaf) const { return path + "/" + leaf; }
};

}  // namespace

// ---- addressing ----------------------------------------------------------

// A reader on another node cannot reach these bytes by path, so the URL is
// the only way to address them -- which is why, unlike the POSIX backend,
// this one always answers `Remote` and always publishes a layers URL.
MT_TEST(blobs_are_addressed_by_url_under_the_configured_prefix) {
    cfg::OssBackendConfig config = GoodConfig();
    config.prefix = core::Optional<std::string>("repo");

    RecordingTransport transport;
    OssSnapshotRepository repository(ClientOf(transport.Fn(), config),
                                     std::shared_ptr<ObjectUploader>());

    MT_EXPECT_TRUE(repository.ManagedLayersRepoBlobUrl().has_value());
    MT_EXPECT_TRUE(repository.ManagedLayersRepoBlobUrl().has_value() &&
                   *repository.ManagedLayersRepoBlobUrl() ==
                       "s3://agentenv/repo/managed-layers");

    const RepositoryResult<BlobLocation> located =
        repository.LocateBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs));
    MT_EXPECT_TRUE(located.ok());
    MT_EXPECT_TRUE(located.ok() && located.value().kind == BlobLocation::Kind::Remote);
    MT_EXPECT_TRUE(located.ok() &&
                   located.value().value == "s3://agentenv/repo/managed-layers/sha256:abc");
    // Locating issues nothing: it is a naming decision, and a round trip per
    // layer would make materialising an image stack cost one request per
    // layer for information the client already has.
    MT_EXPECT_EQ(static_cast<int>(transport.requests.size()), 0);
}

// ---- existence -----------------------------------------------------------

// A HEAD, not a GET: probing a multi-gigabyte layer must not download it.
MT_TEST(has_blob_probes_with_a_head_request) {
    RecordingTransport transport;
    transport.Push(200);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const RepositoryResult<bool> present =
        repository.HasBlob(BlobKey::Snapshot(TestId(), SnapshotFile::VmState));
    MT_EXPECT_TRUE(present.ok() && present.value());
    MT_EXPECT_EQ(static_cast<int>(transport.requests.size()), 1);
    MT_EXPECT_TRUE(transport.requests[0].method == "HEAD");
    MT_EXPECT_TRUE(transport.requests[0].url.find(
                       "/agentenv/artifacts/11111111-2222-3333-4444-555555555555/vm_state.bin") !=
                   std::string::npos);
}

MT_TEST(has_blob_reports_a_missing_object_as_absent) {
    RecordingTransport transport;
    transport.Push(404);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    const RepositoryResult<bool> present =
        repository.HasBlob(BlobKey::Layer("sha256:abc", LayerRole::Memory));
    MT_EXPECT_TRUE(present.ok() && !present.value());
}

// A 403 is not absence. Reporting it as "no" would make a publish skip an
// upload it never verified, and a reader conclude a layer was never
// published -- both of which look like success.
MT_TEST(a_forbidden_existence_probe_is_an_error_rather_than_absence) {
    RecordingTransport transport;
    // Two 403s: the client re-signs once for clock skew before giving up.
    transport.Push(403, "AccessDenied");
    transport.Push(403, "AccessDenied");
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    MT_EXPECT_TRUE(!repository.HasBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs)).ok());
}

// ---- put -----------------------------------------------------------------

// A layer is named by its digest, so a layer already stored holds the same
// bytes by construction. Skipping it is what keeps a re-publish from
// re-sending gigabytes.
MT_TEST(putting_a_layer_that_is_already_stored_skips_the_upload) {
    RecordingTransport transport;
    transport.Push(200);  // HEAD: present
    std::shared_ptr<RecordingUploader> uploader(new RecordingUploader());
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()), uploader);

    const RepositoryResult<core::Unit> put =
        repository.PutBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs),
                           BlobBody::File("/tmp/does-not-need-to-exist"));
    MT_EXPECT_TRUE(put.ok());
    MT_EXPECT_EQ(static_cast<int>(uploader->uploads.size()), 0);
    MT_EXPECT_EQ(static_cast<int>(transport.requests.size()), 1);
    MT_EXPECT_TRUE(transport.requests[0].method == "HEAD");
}

// The upload is addressed by the same URL `LocateBlob` reports, so what was
// written and what readers are pointed at cannot diverge.
MT_TEST(putting_a_missing_layer_uploads_the_file_under_the_located_url) {
    RecordingTransport transport;
    transport.Push(404);  // HEAD: absent
    std::shared_ptr<RecordingUploader> uploader(new RecordingUploader());
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()), uploader);

    const BlobKey key = BlobKey::Layer("sha256:abc", LayerRole::AttachedDrive);
    MT_EXPECT_TRUE(repository.PutBlob(key, BlobBody::File("/var/lib/layer.commit")).ok());
    MT_EXPECT_EQ(static_cast<int>(uploader->uploads.size()), 1);
    MT_EXPECT_TRUE(uploader->uploads.size() == 1 &&
                   uploader->uploads[0].first == "s3://agentenv/managed-layers/sha256:abc");
    MT_EXPECT_TRUE(uploader->uploads.size() == 1 &&
                   uploader->uploads[0].second == "/var/lib/layer.commit");

    const RepositoryResult<BlobLocation> located = repository.LocateBlob(key);
    MT_EXPECT_TRUE(located.ok() && located.value().value == uploader->uploads[0].first);
}

// A snapshot file is replaced, not kept: it is not content-addressed, so the
// existence check a layer gets would skip a re-publish that must land.
MT_TEST(a_snapshot_file_is_put_without_an_existence_check) {
    RecordingTransport transport;
    transport.Push(200);
    std::shared_ptr<RecordingUploader> uploader(new RecordingUploader());
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()), uploader);

    std::vector<uint8_t> bytes;
    bytes.push_back('{');
    bytes.push_back('}');
    MT_EXPECT_TRUE(
        repository
            .PutBlob(BlobKey::Snapshot(TestId(), SnapshotFile::FirecrackerManifest),
                     BlobBody::Bytes(bytes))
            .ok());
    MT_EXPECT_EQ(static_cast<int>(transport.requests.size()), 1);
    MT_EXPECT_TRUE(transport.requests[0].method == "PUT");
    // Unconditional, as upstream: Alibaba OSS rejects `x-oss-forbid-overwrite`
    // on the multipart uploads the file path uses, so neither path can rely on
    // a create-only precondition.
    MT_EXPECT_TRUE(HeaderOf(transport.requests[0], "If-None-Match").empty());
    MT_EXPECT_TRUE(HeaderOf(transport.requests[0], "If-Match").empty());
}

// Without an uploader the file path is refused rather than read into memory:
// a layer is routinely gigabytes, so the byte path would work on a fixture
// and exhaust the node on a real publish.
MT_TEST(a_file_body_without_an_uploader_is_refused) {
    RecordingTransport transport;
    transport.Push(404);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    const RepositoryResult<core::Unit> put =
        repository.PutBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs),
                           BlobBody::File("/var/lib/layer.commit"));
    MT_EXPECT_TRUE(!put.ok());
    MT_EXPECT_TRUE(!put.ok() && put.error().kind == RepositoryErrorKind::Unsupported);
}

// ---- read ----------------------------------------------------------------

MT_TEST(read_blob_returns_the_bytes) {
    RecordingTransport transport;
    transport.Push(200, "{\"manifest\":true}", "\"v1\"");
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    const RepositoryResult<std::string> bytes =
        repository.ReadBlob(BlobKey::Snapshot(TestId(), SnapshotFile::FirecrackerManifest));
    MT_EXPECT_TRUE(bytes.ok() && bytes.value() == "{\"manifest\":true}");
}

// `ArtifactNotFound`, not a backend error: the caller is deciding whether the
// artifact was ever published, and a transport failure would send it to look
// for a broken network instead.
MT_TEST(read_blob_reports_an_absent_object_as_artifact_not_found) {
    RecordingTransport transport;
    transport.Push(404);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    const RepositoryResult<std::string> bytes =
        repository.ReadBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs));
    MT_EXPECT_TRUE(!bytes.ok());
    MT_EXPECT_TRUE(!bytes.ok() && bytes.error().kind == RepositoryErrorKind::ArtifactNotFound);
}

// ---- fetch ---------------------------------------------------------------

MT_TEST(fetch_blob_streams_to_the_destination_and_returns_its_size) {
    TempRoot root;
    const std::string destination = root.At("nested/dir/layer.commit");

    RecordingTransport transport;
    transport.Push(200, "layer-bytes");
    transport.SetContentLength(11);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const RepositoryResult<uint64_t> fetched =
        repository.FetchBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs), destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_TRUE(fetched.ok() && fetched.value() == 11);
    const core::Expected<std::string, std::string> written =
        core::fs::ReadToString(destination);
    MT_EXPECT_TRUE(written.ok() && written.value() == "layer-bytes");
}

// Nothing is created for a 404. An empty file left at the destination is
// indistinguishable from a published zero-length blob, and the caller would
// go on to use it.
MT_TEST(a_fetch_of_a_missing_blob_leaves_no_file_behind) {
    TempRoot root;
    const std::string destination = root.At("layer.commit");

    RecordingTransport transport;
    transport.Push(404);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const RepositoryResult<uint64_t> fetched =
        repository.FetchBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs), destination);
    MT_EXPECT_TRUE(!fetched.ok());
    MT_EXPECT_TRUE(!fetched.ok() && fetched.error().kind == RepositoryErrorKind::ArtifactNotFound);
    MT_EXPECT_TRUE(!core::fs::Exists(destination));
}

// A body shorter than its own `Content-Length` is a truncated transfer.
// Returning its size would publish a layer whose tail is missing, which the
// reader only discovers as a corrupt image, so the partial file goes too.
MT_TEST(a_truncated_download_is_refused_and_its_partial_file_removed) {
    TempRoot root;
    const std::string destination = root.At("layer.commit");

    RecordingTransport transport;
    transport.Push(200, "short");
    transport.SetContentLength(4096);
    OssSnapshotRepository repository(ClientOf(transport.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const RepositoryResult<uint64_t> fetched =
        repository.FetchBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs), destination);
    MT_EXPECT_TRUE(!fetched.ok());
    MT_EXPECT_TRUE(!core::fs::Exists(destination));
}

// A retried download starts the file over. The first attempt died with bytes
// already written, and appending the second response to them would produce a
// file of plausible length holding two spliced responses -- which no size or
// status check downstream would catch.
MT_TEST(a_retried_download_restarts_the_file_instead_of_appending) {
    TempRoot root;
    const std::string destination = root.At("layer.commit");

    int calls = 0;
    const OssClient::Transport transport =
        [&calls](const shttp::Request& request) -> core::Expected<shttp::Response, std::string> {
            ++calls;
            if (calls == 1) {
                // Bytes arrive, then the connection dies mid-body.
                if (request.body_sink != 0) {
                    request.body_sink->Write(reinterpret_cast<const uint8_t*>("half"), 4);
                }
                return core::make_unexpected(std::string("connection reset mid-body"));
            }
            shttp::Response response;
            response.status = 200;
            const std::string body = "complete-payload";
            response.body.assign(body.begin(), body.end());
            return DeliverBody(request, response);
        };

    OssSnapshotRepository repository(ClientOf(transport, GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());
    const RepositoryResult<uint64_t> fetched =
        repository.FetchBlob(BlobKey::Layer("sha256:abc", LayerRole::Rootfs), destination);
    MT_EXPECT_TRUE(fetched.ok());
    MT_EXPECT_TRUE(fetched.ok() && fetched.value() == 16);
    const core::Expected<std::string, std::string> written =
        core::fs::ReadToString(destination);
    MT_EXPECT_TRUE(written.ok() && written.value() == "complete-payload");
}

// ---- documents -----------------------------------------------------------

// The document half is the one that is complete, and it is what makes the
// volume catalog usable over object storage: these are the five calls it
// makes.
MT_TEST(the_document_half_round_trips_through_the_repository) {
    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const DocumentKey key = DocumentKey::VolumeRecord("vol_a");
    MT_EXPECT_TRUE(repository.PutDocument(key, "{\"id\":\"vol_a\"}").ok());

    const RepositoryResult<core::Optional<std::string> > read = repository.GetDocument(key);
    MT_EXPECT_TRUE(read.ok() && read.value().has_value());
    MT_EXPECT_TRUE(read.ok() && read.value().has_value() && *read.value() == "{\"id\":\"vol_a\"}");

    MT_EXPECT_TRUE(repository.PutDocument(DocumentKey::VolumeRecord("vol_b"), "{}").ok());
    const RepositoryResult<std::vector<std::string> > ids =
        repository.ListDocuments(DocumentCollection::VolumeRecords, core::nullopt, 10);
    MT_EXPECT_TRUE(ids.ok() && ids.value().size() == 2);
    MT_EXPECT_TRUE(ids.ok() && ids.value().size() == 2 && ids.value()[0] == "vol_a");
    MT_EXPECT_TRUE(ids.ok() && ids.value().size() == 2 && ids.value()[1] == "vol_b");

    MT_EXPECT_TRUE(repository.DeleteDocument(key).ok());
    const RepositoryResult<core::Optional<std::string> > gone = repository.GetDocument(key);
    MT_EXPECT_TRUE(gone.ok() && !gone.value().has_value());
    // Idempotent, as on every backend: a retry after a partial failure must
    // not start failing.
    MT_EXPECT_TRUE(repository.DeleteDocument(key).ok());
}

// The compare-and-swap reaches the repository boundary with its retry intact:
// the competing write lands between the GET and the PUT, and the decision is
// made again from the winner's bytes rather than discarding them.
//
// Unlike the client-level tests this one runs the *default* policy, so it
// also shows the production schedule is wired up -- one lost race costs a
// single 20 ms backoff.
MT_TEST(update_document_through_the_repository_survives_a_lost_race) {
    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    const DocumentKey key = DocumentKey::VolumeRecord("vol_a");
    server.Seed(key.Path(), "v1");
    server.before_put = [&server, &key]() { server.Seed(key.Path(), "v2-other-node"); };

    std::vector<std::string> seen;
    const RepositoryResult<core::Optional<std::string> > updated = repository.UpdateDocument(
        key, [&seen](const core::Optional<std::string>& current)
                 -> RepositoryResult<DocumentChange> {
            seen.push_back(current.has_value() ? *current : std::string("<absent>"));
            return DocumentChange::Put("v3-mine");
        });

    MT_EXPECT_TRUE(updated.ok());
    MT_EXPECT_TRUE(seen.size() == 2);
    MT_EXPECT_TRUE(seen.size() == 2 && seen[1] == "v2-other-node");
    MT_EXPECT_TRUE(server.bodies[key.Path()] == "v3-mine");
}

// ---- snapshot records ----------------------------------------------------

// The record methods are served by the shared document-backed catalog
// (`record_catalog.h`), so they reach the store rather than refusing. What is
// asserted here is the *forwarding*: that each one issues requests against
// the snapshot-record key space, and -- the part that would be a stack
// overflow if it were wrong -- that forwarding a record method to a layer
// built on the document methods does not re-enter the record method.
//
// The behaviour of that layer is `tests/test_record_catalog.cc`.
MT_TEST(the_record_methods_are_served_through_the_shared_document_catalog) {
    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    SnapshotRecord record;
    record.id = TestId();
    record.source = SnapshotSource::Template(TemplateBuildInfo());
    MT_EXPECT_TRUE(repository.Create(record).ok());

    const RepositoryResult<core::Optional<SnapshotRecord> > got =
        repository.Get(TestId().ToString());
    MT_EXPECT_TRUE(got.ok() && got.value().has_value());
    MT_EXPECT_TRUE(repository.List(SnapshotListFilter::MatchesAll()).ok());
    MT_EXPECT_TRUE(repository.ResolveAlias("missing").ok());
    MT_EXPECT_TRUE(repository.TryStartBuild(TestId()).ok());
    MT_EXPECT_TRUE(repository.MarkBuildError(TestId(), TemplateBuildErrorReason()).ok());

    // Under the shared key space, so a node on another backend reads the same
    // document.
    MT_EXPECT_TRUE(server.bodies.count("catalog/records/" + TestId().ToString() + ".json") == 1);
}

// `Delete` removes the record *and* the snapshot's objects. The second half
// is this backend's own (`delete_snapshot_blobs` -> list the snapshot prefix,
// delete each key); the ordering and the marking are the shared layer's and
// are tested there.
MT_TEST(delete_removes_the_record_and_the_snapshots_own_objects) {
    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    SnapshotRecord record;
    record.id = TestId();
    record.source = SnapshotSource::Template(TemplateBuildInfo());
    MT_EXPECT_TRUE(repository.Create(record).ok());

    const std::string vm_state = BlobKey::Snapshot(TestId(), SnapshotFile::VmState).Path();
    server.Seed(vm_state, "state-bytes");
    // A managed layer is content-addressed and lives outside the snapshot's
    // own prefix, so it must survive: another snapshot may share it.
    const std::string layer = BlobKey::Layer("sha256:abc", LayerRole::Rootfs).Path();
    server.Seed(layer, "layer-bytes");
    // A sibling snapshot whose id merely *starts with* the same text. Without
    // the trailing slash on the delete prefix this one would go too.
    const std::string sibling = vm_state;
    server.Seed(BlobKey::SnapshotDir(TestId()) + "-other/vm_state.bin", "other-bytes");

    MT_EXPECT_TRUE(repository.Delete(TestId().ToString()).ok());

    MT_EXPECT_TRUE(server.bodies.count("catalog/records/" + TestId().ToString() + ".json") == 0);
    MT_EXPECT_TRUE(server.bodies.count(sibling) == 0);
    MT_EXPECT_TRUE(server.bodies.count(layer) == 1);
    MT_EXPECT_TRUE(
        server.bodies.count(BlobKey::SnapshotDir(TestId()) + "-other/vm_state.bin") == 1);

    // Idempotent: a delete that was interrupted must be retryable.
    MT_EXPECT_TRUE(repository.Delete(TestId().ToString()).ok());
}

// The same delete under a configured prefix. `ListKeysRecursive` hands back
// repository-relative keys and `Delete` re-applies the prefix, so a key that
// goes through unchanged -- or gets prefixed twice -- addresses nothing and
// the delete reports success having removed nothing. With no prefix set that
// mistake is invisible, which is why this case is asserted on its own.
MT_TEST(delete_removes_the_snapshots_objects_under_a_configured_prefix) {
    cfg::OssBackendConfig config = GoodConfig();
    config.prefix = core::Optional<std::string>("repo");

    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), config),
                                     std::shared_ptr<ObjectUploader>());

    SnapshotRecord record;
    record.id = TestId();
    record.source = SnapshotSource::Template(TemplateBuildInfo());
    MT_EXPECT_TRUE(repository.Create(record).ok());

    const std::string record_key = "repo/catalog/records/" + TestId().ToString() + ".json";
    const std::string vm_state =
        "repo/" + BlobKey::Snapshot(TestId(), SnapshotFile::VmState).Path();
    server.Seed(vm_state, "state-bytes");
    MT_EXPECT_TRUE(server.bodies.count(record_key) == 1);

    MT_EXPECT_TRUE(repository.Delete(TestId().ToString()).ok());
    MT_EXPECT_TRUE(server.bodies.count(vm_state) == 0);
    MT_EXPECT_TRUE(server.bodies.count(record_key) == 0);
}

// A published disk image is rolled back, then removed. The rollback here has
// no credentials to work with, by construction, and the delete still finishes:
// a registry that cannot be reached must not pin the record forever, since the
// record is the only thing that makes the delete retryable.
MT_TEST(delete_proceeds_for_a_published_disk_image) {
    const agentenv::testing::NoDockerCredentials no_credentials;
    StubObjectServer server;
    OssSnapshotRepository repository(ClientOf(server.Fn(), GoodConfig()),
                                     std::shared_ptr<ObjectUploader>());

    SnapshotRecord record;
    record.id = TestId();
    record.source = SnapshotSource::Template(TemplateBuildInfo());
    CommittedSnapshot committed;
    PersistedDiskImagePublication publication;
    publication.image_ref = "registry.example.com/env";
    publication.tag = "1";
    publication.manifest_digest = "sha256:" + std::string(64, 'a');
    publication.repo_blob_url = "https://registry.example.com/v2/env/blobs/";
    committed.disk_publications.push_back(publication);
    record.committed = core::Optional<CommittedSnapshot>(committed);
    // Written directly: `Create` refuses an already-committed record.
    server.Seed("catalog/records/" + TestId().ToString() + ".json",
                record.ToJson().ToStringPretty());

    MT_EXPECT_TRUE(repository.Delete(TestId().ToString()).ok());

    const RepositoryResult<core::Optional<SnapshotRecord> > gone =
        repository.Get(TestId().ToString());
    MT_EXPECT_TRUE(gone.ok() && !gone.value().has_value());
}

// ---- the factory ---------------------------------------------------------

// Validation precedes connection, so an incomplete section is reported as
// itself rather than as a 403 on the first signed request.
/// The compression config the factory takes. Passthrough, so a test that does
/// not care about the policy cannot accidentally depend on compression being
/// on -- the config struct's *own* default has `enabled = true`.
cfg::SnapshotPublishCompressionConfig NoCompression() {
    cfg::SnapshotPublishCompressionConfig config;
    config.enabled = false;
    return config;
}

MT_TEST(the_factory_refuses_an_incomplete_backend_section) {
    cfg::OssBackendConfig blank;
    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeOssRepository(blank, NoCompression());
    MT_EXPECT_TRUE(!made.ok());
    MT_EXPECT_TRUE(!made.ok() && made.error().kind == RepositoryErrorKind::InvalidRequest);

    cfg::OssBackendConfig process = GoodConfig();
    process.credential_process = core::Optional<std::string>("/usr/bin/aliyun-cred");
    const RepositoryResult<std::shared_ptr<SnapshotRepository> > refused =
        MakeOssRepository(process, NoCompression());
    MT_EXPECT_TRUE(!refused.ok() && refused.error().kind == RepositoryErrorKind::Unsupported);
}

// A complete section yields a repository that addresses its own bucket. The
// assertion is conditional on libcurl because without it the factory
// correctly refuses to build something that could never send a request.
MT_TEST(the_factory_builds_a_repository_from_a_complete_section) {
    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeOssRepository(GoodConfig(), NoCompression());
    if (!shttp::Available()) {
        MT_EXPECT_TRUE(!made.ok() && made.error().kind == RepositoryErrorKind::Unsupported);
        return;
    }
    MT_EXPECT_TRUE(made.ok());
    MT_EXPECT_TRUE(made.ok() && made.value()->ManagedLayersRepoBlobUrl().has_value());
    MT_EXPECT_TRUE(made.ok() && *made.value()->ManagedLayersRepoBlobUrl() ==
                                    "s3://agentenv/managed-layers");
    // Built but not used: every operation past construction needs the bucket.
}

// ---- the publish policy --------------------------------------------------

/// The operator's compression switch reaches the repository's policy.
///
/// This is the whole point of the OSS override: the switch is read where the
/// backend is constructed, and `store.cc` asks the repository rather than the
/// config. All three fields are checked, because carrying `enabled` across
/// while dropping the algorithm would publish lz4 where zstd was configured.
MT_TEST(the_factory_carries_the_compression_switch_into_the_publish_policy) {
    cfg::SnapshotPublishCompressionConfig configured;
    configured.enabled = true;
    configured.algorithm = cfg::OverlaybdCompressionAlgorithm::Zstd;
    configured.workers = 3;

    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeOssRepository(GoodConfig(), configured);
    if (!shttp::Available()) {
        MT_EXPECT_TRUE(!made.ok());
        return;
    }
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;

    const PublishPolicy policy = made.value()->GetPublishPolicy();
    MT_EXPECT_TRUE(policy.compression.enabled);
    MT_EXPECT_TRUE(policy.compression.algorithm == cfg::OverlaybdCompressionAlgorithm::Zstd);
    MT_EXPECT_TRUE(policy.compression.workers == 3);
}

/// A repository that does not override the policy publishes raw.
///
/// Asserted against the base class' default body, which is what every backend
/// but OSS inherits. The value under test is `enabled == false`, and it is
/// *not* the config struct's default -- that one is `true`. Getting this
/// wrong would turn every posix-fs and in-memory publish into lz4
/// compression, so the distinction is pinned here rather than left to the
/// comment that explains it.
MT_TEST(a_backend_without_a_policy_override_publishes_raw) {
    InMemorySnapshotRepository plain;
    const PublishPolicy policy = plain.GetPublishPolicy();
    MT_EXPECT_TRUE(!policy.compression.enabled);
    // The config default is the opposite, which is why the policy sets it
    // explicitly instead of default-constructing the member.
    MT_EXPECT_TRUE(cfg::SnapshotPublishCompressionConfig().enabled);
}

// ---- backend selection ---------------------------------------------------

/// The declared backend selects the repository, **not** which sections are
/// present.
///
/// This is the regression test for the bug the dispatcher was written to fix:
/// config normalisation only auto-inserts `[backend.posix_fs]` when the
/// backend is posix-fs, but an operator migrating a node to OSS ordinarily
/// leaves the old section in place. The startup path used to branch on that
/// section's presence, so such a node published to its local disk while its
/// config said `oss`. Here both sections are present and the declared backend
/// is OSS, which is exactly that configuration.
MT_TEST(the_declared_backend_wins_over_a_leftover_posixfs_section) {
    TempRoot root;
    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::Oss;
    config.backend.oss = core::Optional<cfg::OssBackendConfig>(GoodConfig());
    // The leftover section a migrated node still carries.
    cfg::PosixFsBackendConfig posix;
    posix.snapshot_store = root.path;
    config.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>(posix);

    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeRepositoryFromConfig(config);
    if (!shttp::Available()) {
        // Without libcurl an OSS repository cannot be built at all, which is
        // itself the right answer -- and still not a silent fall back to the
        // posix-fs section.
        MT_EXPECT_TRUE(!made.ok());
        return;
    }
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;
    // An OSS repository names managed layers by URL; the posix-fs one has no
    // such URL, so this is what tells the two apart.
    MT_EXPECT_TRUE(made.value()->ManagedLayersRepoBlobUrl().has_value());
}

/// posix-fs is selected when declared, and is *not* an OSS repository.
MT_TEST(the_posixfs_backend_is_selected_when_declared) {
    TempRoot root;
    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::PosixFs;
    cfg::PosixFsBackendConfig posix;
    posix.snapshot_store = root.path;
    config.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>(posix);
    // An OSS section present but not declared must be ignored, the mirror of
    // the test above.
    config.backend.oss = core::Optional<cfg::OssBackendConfig>(GoodConfig());

    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeRepositoryFromConfig(config);
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;
    MT_EXPECT_TRUE(!made.value()->ManagedLayersRepoBlobUrl().has_value());
    // A real posix-fs repository serves records; the in-memory stub the old
    // removed factory returned would too, so the URL check above is what
    // distinguishes the backend and this confirms it is functional.
    MT_EXPECT_TRUE(made.value()->List(SnapshotListFilter::MatchesAll()).ok());
}

/// A missing section for the declared backend is an error, not a silent
/// switch to the other one.
MT_TEST(a_missing_section_for_the_declared_backend_is_an_error) {
    cfg::AppConfig oss_without_section;
    oss_without_section.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::Oss;
    // Deliberately *with* a posix-fs section, so a fallback would succeed and
    // this assertion is about refusing rather than about having no option.
    cfg::PosixFsBackendConfig posix;
    posix.snapshot_store = "/tmp/agentenv-unused";
    oss_without_section.backend.posix_fs = core::Optional<cfg::PosixFsBackendConfig>(posix);

    const RepositoryResult<std::shared_ptr<SnapshotRepository> > refused =
        MakeRepositoryFromConfig(oss_without_section);
    MT_EXPECT_TRUE(!refused.ok());
    if (!refused.ok()) {
        MT_EXPECT_TRUE(refused.error().kind == RepositoryErrorKind::InvalidRequest);
        MT_EXPECT_TRUE(refused.error().ToString().find("backend.oss config is required") !=
                       std::string::npos);
    }

    cfg::AppConfig posixfs_without_section;
    posixfs_without_section.snapshot.repository_backend =
        cfg::SnapshotRepositoryBackendKind::PosixFs;
    const RepositoryResult<std::shared_ptr<SnapshotRepository> > also_refused =
        MakeRepositoryFromConfig(posixfs_without_section);
    MT_EXPECT_TRUE(!also_refused.ok());
    if (!also_refused.ok()) {
        MT_EXPECT_TRUE(also_refused.error().ToString().find(
                           "backend.posix_fs config is required") != std::string::npos);
    }
}

/// The compression switch reaches the repository through the dispatcher too,
/// not only through `MakeOssRepository` called directly.
MT_TEST(the_dispatcher_carries_the_compression_switch_through) {
    cfg::AppConfig config;
    config.snapshot.repository_backend = cfg::SnapshotRepositoryBackendKind::Oss;
    config.backend.oss = core::Optional<cfg::OssBackendConfig>(GoodConfig());
    config.snapshot.publish_compression.enabled = true;
    config.snapshot.publish_compression.algorithm = cfg::OverlaybdCompressionAlgorithm::Zstd;
    config.snapshot.publish_compression.workers = 5;

    const RepositoryResult<std::shared_ptr<SnapshotRepository> > made =
        MakeRepositoryFromConfig(config);
    if (!shttp::Available()) {
        MT_EXPECT_TRUE(!made.ok());
        return;
    }
    MT_EXPECT_TRUE(made.ok());
    if (!made.ok()) return;
    const PublishPolicy policy = made.value()->GetPublishPolicy();
    MT_EXPECT_TRUE(policy.compression.enabled);
    MT_EXPECT_TRUE(policy.compression.algorithm == cfg::OverlaybdCompressionAlgorithm::Zstd);
    MT_EXPECT_TRUE(policy.compression.workers == 5);
}

MT_MAIN
