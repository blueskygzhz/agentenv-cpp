// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/publisher.rs — `validates_oci_snapshot_tags`.
//
// The tag is checked before any blob is uploaded. A tag rejected late would
// leave the blobs pushed with no manifest naming them, so the next publish
// re-uploads them and nothing ever collects the orphans — which is why these
// rules are pinned rather than left to the registry's 400.
#include "agentenv/snapshot/image_export/acr_publisher.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/acr_manifest.h"
#include "agentenv/snapshot/image_export/acr.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/sys.h"
#include "microtest.h"

using namespace agentenv;                           // NOLINT
using namespace agentenv::snapshot;                 // NOLINT
using namespace agentenv::snapshot::image_export;   // NOLINT
using namespace agentenv::snapshot::repository;     // NOLINT

namespace obdhttp = agentenv::storage::overlaybd::http;
namespace obd = agentenv::storage::overlaybd;

namespace {

core::SnapshotId FixtureId() {
    core::Uuid uuid;
    MT_EXPECT_TRUE(core::Uuid::Parse("018f0d93-aaaa-bbbb-cccc-0123456789ab", &uuid));
    return core::SnapshotId(uuid);
}

bool Accepts(const std::string& tag) { return ValidateSnapshotTag(tag).ok(); }

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// ValidateSnapshotTag
// ---------------------------------------------------------------------------

MT_TEST(a_generated_snapshot_tag_is_a_valid_oci_tag) {
    MT_EXPECT_TRUE(Accepts("agentenv-snapshot-018f0d93-aaaa-bbbb-cccc-0123456789ab"));
    MT_EXPECT_TRUE(Accepts("_leading_underscore"));
}

/// `-` may not lead, though it is allowed afterwards: a leading `-` would be
/// read as a flag by the registry tooling a tag is pasted into. That is the
/// whole reason the start and continuation rules differ.
MT_TEST(a_tag_may_not_start_with_a_dash_or_a_dot_but_may_contain_them) {
    MT_EXPECT_TRUE(!Accepts("-bad"));
    MT_EXPECT_TRUE(!Accepts(".bad"));
    MT_EXPECT_TRUE(Accepts("a-b.c_d"));
    // A digit or `_` start is fine.
    MT_EXPECT_TRUE(Accepts("0leading-digit"));
}

MT_TEST(a_tag_with_a_separator_is_refused) {
    // `/` would change which repository the manifest is addressed to, and `:`
    // would split off a second tag.
    MT_EXPECT_TRUE(!Accepts("bad/tag"));
    MT_EXPECT_TRUE(!Accepts("bad:tag"));
    MT_EXPECT_TRUE(!Accepts("bad tag"));
    MT_EXPECT_TRUE(!Accepts("bad@tag"));
}

MT_TEST(the_length_limit_is_128_and_an_empty_tag_is_refused) {
    MT_EXPECT_TRUE(Accepts(std::string(128, 'a')));
    MT_EXPECT_TRUE(!Accepts(std::string(129, 'a')));
    // Empty has no first character to check, and upstream refuses it there
    // rather than letting the loop pass vacuously.
    MT_EXPECT_TRUE(!Accepts(""));
}

/// Non-ASCII is refused. Pinned because the limit is a *byte* count upstream
/// (`str::len()`), so a tag of multi-byte characters must not slip under it on
/// a character count.
MT_TEST(a_non_ascii_tag_is_refused) {
    MT_EXPECT_TRUE(!Accepts("caf\xc3\xa9"));
    MT_EXPECT_TRUE(!Accepts("\xe6\x95\xb0\xe6\x8d\xae"));
    // 50 three-byte characters: 150 bytes, 50 characters. Refused either way,
    // but it must be refused.
    std::string wide;
    for (int i = 0; i < 50; ++i) wide += "\xe6\x95\xb0";
    MT_EXPECT_TRUE(!Accepts(wide));
}

MT_TEST(the_refusal_names_the_tag_and_is_an_invalid_request) {
    const RepositoryResult<core::Unit> out = ValidateSnapshotTag("bad/tag");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        // InvalidRequest, not Backend: the caller fixes this by passing a
        // different tag, and a retry of the same one can never succeed.
        MT_EXPECT_TRUE(out.error().kind == RepositoryErrorKind::InvalidRequest);
        MT_EXPECT_TRUE(out.error().ToString().find("bad/tag") != std::string::npos);
        MT_EXPECT_TRUE(out.error().ToString().find("not a valid OCI tag") !=
                       std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// SnapshotTag
// ---------------------------------------------------------------------------

MT_TEST(the_rootfs_subject_tags_with_the_plain_snapshot_tag) {
    const core::SnapshotId id = FixtureId();
    MT_EXPECT_EQ(SnapshotTag(id, DiskImageSubject::Rootfs()),
                 agentenv::snapshot::RootfsSnapshotImageTag(id));
    MT_EXPECT_EQ(SnapshotTag(id, DiskImageSubject::Rootfs()),
                 std::string("agentenv-snapshot-018f0d93-aaaa-bbbb-cccc-0123456789ab"));
}

MT_TEST(an_attached_drive_tag_is_the_rootfs_tag_plus_a_sanitized_drive_suffix) {
    const core::SnapshotId id = FixtureId();
    const std::string tag = SnapshotTag(id, DiskImageSubject::AttachedDrive("data:cache"));
    MT_EXPECT_TRUE(StartsWith(
        tag, agentenv::snapshot::RootfsSnapshotImageTag(id) + "-drive-data-cache-"));
    // Whatever the drive id was, the result must still be publishable.
    MT_EXPECT_TRUE(Accepts(tag));
}

/// Every generated tag has to pass the validator, including for ids built from
/// characters a tag may not contain. A drive id is operator-supplied, so this
/// is the case that actually occurs.
MT_TEST(generated_drive_tags_are_always_valid_tags) {
    const core::SnapshotId id = FixtureId();
    const char* const drive_ids[] = {
        "data:cache", "data/cache", "/abs/path", "-leading-dash", ".leading-dot",
        "with space", "UPPER_case", "sym#$%^&*()", "",
        "0123456789012345678901234567890123456789",  // longer than the 32 cap
        "caf\xc3\xa9",                                // non-ASCII
        "\xe6\x95\xb0\xe6\x8d\xae",                   // all non-ASCII
    };
    for (std::size_t i = 0; i < sizeof(drive_ids) / sizeof(drive_ids[0]); ++i) {
        const std::string tag = SnapshotTag(id, DiskImageSubject::AttachedDrive(drive_ids[i]));
        MT_EXPECT_TRUE(Accepts(tag));
        if (!Accepts(tag)) std::printf("  drive_id=[%s] tag=[%s]\n", drive_ids[i], tag.c_str());
    }
}

// ---------------------------------------------------------------------------
// SanitizeDriveTagComponent
// ---------------------------------------------------------------------------

/// The hash is computed over the *original* id, so ids that sanitise onto the
/// same text stay distinct. Without it, two drives of one snapshot would
/// publish to the same tag and the second would overwrite the first.
MT_TEST(ids_that_sanitize_alike_still_get_different_tags) {
    const std::string colon = SanitizeDriveTagComponent("data:cache");
    const std::string slash = SanitizeDriveTagComponent("data/cache");
    const std::string dash = SanitizeDriveTagComponent("data-cache");
    // All three sanitise to the same visible text...
    MT_EXPECT_EQ(colon.substr(0, 10), std::string("data-cache"));
    MT_EXPECT_EQ(slash.substr(0, 10), std::string("data-cache"));
    MT_EXPECT_EQ(dash.substr(0, 10), std::string("data-cache"));
    // ...and are still three different tags.
    MT_EXPECT_TRUE(colon != slash);
    MT_EXPECT_TRUE(slash != dash);
    MT_EXPECT_TRUE(colon != dash);
}

/// The 32-character cap applies to the sanitised text only; the hash suffix is
/// always appended, which is what keeps long ids sharing a prefix distinct.
MT_TEST(a_long_id_is_truncated_to_32_characters_and_still_disambiguated) {
    const std::string a = SanitizeDriveTagComponent(std::string(40, 'a') + "-one");
    const std::string b = SanitizeDriveTagComponent(std::string(40, 'a') + "-two");
    MT_EXPECT_EQ(a.substr(0, 32), std::string(32, 'a'));
    MT_EXPECT_EQ(b.substr(0, 32), std::string(32, 'a'));
    // 32 sanitised + "-" + 12 hex.
    MT_EXPECT_EQ(a.size(), static_cast<std::size_t>(45));
    MT_EXPECT_TRUE(a != b);
}

MT_TEST(an_empty_drive_id_becomes_drive_plus_its_hash) {
    const std::string out = SanitizeDriveTagComponent("");
    MT_EXPECT_EQ(out, std::string("drive-") + core::Sha256Hex(std::string()).substr(0, 12));
}

/// One character in, one `-` out — even for a multi-byte one. Iterating bytes
/// instead would turn a three-byte character into three dashes, so the same
/// drive id would publish under a different tag than upstream.
MT_TEST(a_multi_byte_character_collapses_to_exactly_one_dash) {
    // U+6570 is three bytes; the result must be one dash, not three.
    MT_EXPECT_EQ(SanitizeDriveTagComponent("\xe6\x95\xb0").substr(0, 1), std::string("-"));
    MT_EXPECT_EQ(SanitizeDriveTagComponent("a\xe6\x95\xb0" "b").substr(0, 3),
                 std::string("a-b"));
    // Two-byte (U+00E9) and four-byte (U+1F600) sequences likewise.
    MT_EXPECT_EQ(SanitizeDriveTagComponent("caf\xc3\xa9").substr(0, 4), std::string("caf-"));
    MT_EXPECT_EQ(SanitizeDriveTagComponent("a\xf0\x9f\x98\x80" "b").substr(0, 3),
                 std::string("a-b"));

    // And the cap counts output characters, so 40 multi-byte characters give
    // 32 dashes rather than being cut mid-sequence.
    std::string wide;
    for (int i = 0; i < 40; ++i) wide += "\xe6\x95\xb0";
    MT_EXPECT_EQ(SanitizeDriveTagComponent(wide).substr(0, 32), std::string(32, '-'));
}

/// Characters a tag already allows are passed through untouched, so a
/// well-formed drive id stays readable in the published tag.
MT_TEST(allowed_characters_survive_sanitisation) {
    MT_EXPECT_EQ(SanitizeDriveTagComponent("data_1.v-2").substr(0, 10),
                 std::string("data_1.v-2"));
}

// ---------------------------------------------------------------------------
// DiskImageSubject
// ---------------------------------------------------------------------------

MT_TEST(the_subject_log_label_distinguishes_the_two_kinds) {
    MT_EXPECT_EQ(std::string(DiskImageSubject::Rootfs().LogLabel()), std::string("rootfs"));
    MT_EXPECT_EQ(std::string(DiskImageSubject::AttachedDrive("d").LogLabel()),
                 std::string("attached_drive"));
}

/// `Rootfs` carries no drive id, so a stale one left by a reassignment must
/// not take part in equality.
MT_TEST(a_rootfs_subject_ignores_a_stale_drive_id) {
    DiskImageSubject subject = DiskImageSubject::AttachedDrive("data");
    subject = DiskImageSubject::Rootfs();
    MT_EXPECT_TRUE(subject == DiskImageSubject::Rootfs());
    MT_EXPECT_TRUE(subject != DiskImageSubject::AttachedDrive("data"));
    MT_EXPECT_TRUE(DiskImageSubject::AttachedDrive("a") !=
                   DiskImageSubject::AttachedDrive("b"));
}

// ---------------------------------------------------------------------------
// AcrDiskImageExporter::Export
// ---------------------------------------------------------------------------
//
// What these pin is the *orchestration*, not the pieces: the pieces have their
// own tests. An export is a sequence with a safety order baked into it — plan,
// validate the tag, check the tag is free, push blobs, push the manifest last
// — and only asserting the recorded request sequence can tell a correct export
// from one that transferred the right bytes in the wrong order. The manifest
// landing before its blobs, or a tag check happening after an upload, are both
// invisible to an outcome-only test.

namespace {

const char* const kRegistry = "registry.example";
const char* const kSourceBlobs = "https://registry.example/v2/ns/base/blobs";
const char* const kUploads = "https://registry.example/v2/ns/base/blobs/uploads/";

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_acrexp_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    MT_EXPECT_TRUE(made != 0);
    return made == 0 ? std::string() : std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

/// `lowers` is raw JSON array text, so a test can express a lower shape the
/// typed config would not let it omit.
std::string WriteImage(const std::string& dir, const std::string& lowers) {
    const std::string path = dir + "/image.json";
    MT_EXPECT_TRUE(core::fs::Write(path, "{\"repoBlobUrl\":\"" + std::string(kSourceBlobs) +
                                             "\",\"lowers\":" + lowers +
                                             ",\"upper\":{},\"resultFile\":\"\"}")
                       .ok());
    return path;
}

/// Records every request and answers from a queued script.
///
/// A queue, not a URL map: the order is the thing under test, and a map would
/// let an export that pushed the manifest first still pass.
struct ScriptedTransport {
    struct Reply {
        long status;
        obdhttp::HeaderMap headers;
        Reply() : status(200) {}
    };

    std::vector<Reply> replies;
    std::size_t next;
    std::vector<obdhttp::Request> requests;

    ScriptedTransport() : next(0) {}

    static Reply Status(long status) {
        Reply reply;
        reply.status = status;
        return reply;
    }

    static Reply Session(const std::string& location) {
        Reply reply;
        reply.status = 202;
        reply.headers["Location"] = location;
        return reply;
    }

    static Reply Manifest(const std::string& digest) {
        Reply reply;
        reply.status = 201;
        reply.headers["Docker-Content-Digest"] = digest;
        return reply;
    }

    AcrClient::Transport Fn() {
        ScriptedTransport* self = this;
        return [self](const obdhttp::Request& request)
                   -> core::Expected<obdhttp::Response, std::string> {
            self->requests.push_back(request);
            if (self->next >= self->replies.size()) {
                return core::make_unexpected(std::string("scripted transport exhausted"));
            }
            const Reply& reply = self->replies[self->next++];
            obdhttp::Response response;
            response.status = reply.status;
            response.headers = reply.headers;
            return response;
        };
    }

    std::string MethodOf(std::size_t i) const { return requests[i].method; }
    std::string UrlOf(std::size_t i) const { return requests[i].url; }
    std::string BodyOf(std::size_t i) const {
        return std::string(requests[i].body.begin(), requests[i].body.end());
    }
    /// How many requests used `method`, which is how "nothing was uploaded" is
    /// asserted without depending on the exact count of the rest.
    std::size_t CountOf(const std::string& method) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (requests[i].method == method) ++n;
        }
        return n;
    }
};

AcrDiskImageExporter::ClientBuilder BuilderFor(ScriptedTransport* transport) {
    AcrClient::Transport fn = transport->Fn();
    return [fn](const std::string& registry)
               -> core::Expected<AcrClient, AcrClientError> {
        MT_EXPECT_EQ(registry, std::string(kRegistry));
        DockerRegistryCredentials credentials;
        credentials.username = "alice";
        credentials.password = "pw";
        AcrClientOptions options;
        // Never reached with these scripts; small so a stray retry cannot
        // make the suite sleep.
        options.retry_initial_backoff_ms = 1;
        return AcrClient(core::Optional<DockerRegistryCredentials>(credentials), options, fn);
    };
}

overlaybd::OverlaybdCompactOutput ZFileMode() {
    return overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 1);
}

core::Optional<core::Json> ParseJson(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text);
    if (!parsed.ok()) return core::Optional<core::Json>();
    return core::Optional<core::Json>(parsed.value());
}

/// The virtual size of the sparse fixture below. 1 MiB rather than overlaybd's
/// 64 MiB default: the dense rewrite materialises every byte of it, and that
/// whole thing then travels through the recorded request.
const uint64_t kSparseVirtualSize = 1024ull * 1024;

/// A sparse LSMT layer: one page written at offset 0 of a 1 MiB virtual disk.
///
/// The stored bytes and the bytes a push has to send differ here, which is the
/// entire point — this is the one fixture where no honest descriptor can be
/// recorded up front.
bool BuildSparseLayer(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> data =
        obd::LocalFile::New(path);
    if (!data.ok()) return false;
    const core::Expected<std::shared_ptr<obd::LsmtFile>, std::string> sparse =
        obd::LsmtFile::Create(data.value(), std::shared_ptr<obd::VirtualFile>(),
                              kSparseVirtualSize, true);
    if (!sparse.ok()) return false;
    const std::vector<uint8_t> page(4096, 0xAB);
    if (!sparse.value()->WriteAt(0, &page[0], page.size()).ok()) return false;
    if (!sparse.value()->CloseSeal().ok()) return false;
    return true;
}

std::string ReadWholeFile(const std::string& path) {
    const core::Expected<std::string, std::string> out = core::fs::ReadToString(path);
    return out.ok() ? out.value() : std::string();
}

}  // namespace

/// An all-remote image is a re-tag: every layer is already in the registry, so
/// publishing only has to name them. Re-uploading them would transfer the
/// whole image on every snapshot.
MT_TEST(export_names_remote_lowers_without_uploading_them) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(
        dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
             "{\"digest\":\"sha256:second\",\"size\":456}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));            // tag free
    transport.replies.push_back(ScriptedTransport::Status(404));            // config absent
    transport.replies.push_back(ScriptedTransport::Session("/uploads/c"));  // config session
    transport.replies.push_back(ScriptedTransport::Status(201));            // config PUT
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:fromregistry"));
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  note: %s\n", out.error().ToString().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    // No layer blob was uploaded: the only PATCH-free session here is the
    // config's, so a layer upload would show up as an extra one.
    MT_EXPECT_EQ(transport.CountOf("PATCH"), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(transport.CountOf("POST"), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(5));

    // The layers come back as External, in image order, pointing at the
    // source repository.
    MT_EXPECT_EQ(out.value().layers.size(), static_cast<std::size_t>(2));
    if (out.value().layers.size() == 2) {
        MT_EXPECT_TRUE(out.value().layers[0].is_external());
        MT_EXPECT_EQ(out.value().layers[0].external.digest, std::string("sha256:base"));
        MT_EXPECT_EQ(out.value().layers[0].external.size, static_cast<uint64_t>(123));
        MT_EXPECT_EQ(out.value().layers[0].external.repo_blob_url, std::string(kSourceBlobs));
        MT_EXPECT_EQ(out.value().layers[1].external.digest, std::string("sha256:second"));
    }

    const std::string tag = SnapshotTag(FixtureId(), DiskImageSubject::Rootfs());
    MT_EXPECT_TRUE(out.value().publication.has_value());
    if (out.value().publication.has_value()) {
        MT_EXPECT_EQ(out.value().publication->tag, tag);
        MT_EXPECT_EQ(out.value().publication->image_ref,
                     std::string("registry.example/ns/base:") + tag);
        MT_EXPECT_EQ(out.value().publication->repo_blob_url, std::string(kSourceBlobs));
        // The registry's digest, which is what a rollback will DELETE.
        MT_EXPECT_EQ(out.value().publication->manifest_digest,
                     std::string("sha256:fromregistry"));
    }
    RemoveTreeBestEffort(dir);
}

/// The full sequence for an image with one local delta, asserted request by
/// request. The safety order is the content of this test.
MT_TEST(export_pushes_blobs_before_the_manifest_in_order) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(core::fs::Write(delta, "delta-bytes").ok());
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));            // 0 tag free
    transport.replies.push_back(ScriptedTransport::Status(404));            // 1 delta absent
    transport.replies.push_back(ScriptedTransport::Session("/uploads/l"));  // 2 layer session
    transport.replies.push_back(ScriptedTransport::Status(202));            // 3 layer PATCH
    transport.replies.push_back(ScriptedTransport::Status(201));            // 4 layer PUT
    transport.replies.push_back(ScriptedTransport::Status(404));            // 5 config absent
    transport.replies.push_back(ScriptedTransport::Session("/uploads/c"));  // 6 config session
    transport.replies.push_back(ScriptedTransport::Status(201));            // 7 config PUT
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));  // 8 manifest
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  note: %s\n", out.error().ToString().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(9));

    const std::string tag = SnapshotTag(FixtureId(), DiskImageSubject::Rootfs());
    const std::string manifest_url =
        "https://registry.example/v2/ns/base/manifests/" + tag;
    const std::string delta_digest = core::Sha256Digest(std::string("delta-bytes"));

    // 0: the tag check, before anything is uploaded.
    MT_EXPECT_EQ(transport.MethodOf(0), std::string("HEAD"));
    MT_EXPECT_EQ(transport.UrlOf(0), manifest_url);
    // 1: the delta's existence check, aimed at its digest.
    MT_EXPECT_EQ(transport.MethodOf(1), std::string("HEAD"));
    MT_EXPECT_EQ(transport.UrlOf(1), std::string(kSourceBlobs) + "/" + delta_digest);
    // 2-4: the chunked upload of exactly the delta's bytes.
    MT_EXPECT_EQ(transport.MethodOf(2), std::string("POST"));
    MT_EXPECT_EQ(transport.UrlOf(2), std::string(kUploads));
    MT_EXPECT_EQ(transport.MethodOf(3), std::string("PATCH"));
    MT_EXPECT_EQ(transport.BodyOf(3), std::string("delta-bytes"));
    MT_EXPECT_EQ(transport.MethodOf(4), std::string("PUT"));
    MT_EXPECT_TRUE(transport.UrlOf(4).find("digest=" + delta_digest) != std::string::npos);
    // 5-7: the config blob, after the layers.
    MT_EXPECT_EQ(transport.MethodOf(5), std::string("HEAD"));
    MT_EXPECT_EQ(transport.MethodOf(6), std::string("POST"));
    MT_EXPECT_EQ(transport.MethodOf(7), std::string("PUT"));
    // 8: the manifest, last. Until it lands the blobs are unreferenced, so a
    // failure anywhere above leaves nothing a puller can see.
    MT_EXPECT_EQ(transport.MethodOf(8), std::string("PUT"));
    MT_EXPECT_EQ(transport.UrlOf(8), manifest_url);
    RemoveTreeBestEffort(dir);
}

/// The manifest names the *uploaded* bytes, in image order, and carries the
/// publication tag. A manifest built from the planner's descriptor instead
/// would be accepted by the registry and verifiable by nobody.
MT_TEST(export_builds_a_manifest_naming_the_uploaded_bytes) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(core::fs::Write(delta, "delta-bytes").ok());
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Session("/uploads/l"));
    transport.replies.push_back(ScriptedTransport::Status(202));
    transport.replies.push_back(ScriptedTransport::Status(201));
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Session("/uploads/c"));
    transport.replies.push_back(ScriptedTransport::Status(201));
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Optional<core::Json> manifest = ParseJson(transport.BodyOf(8));
    MT_EXPECT_TRUE(manifest.has_value());
    if (!manifest.has_value()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::JsonObject& fields = manifest->as_object();
    MT_EXPECT_TRUE(fields.find("layers") != fields.end());

    const core::JsonArray& layers = fields.find("layers")->second.as_array();
    MT_EXPECT_EQ(layers.size(), static_cast<std::size_t>(2));
    if (layers.size() == 2) {
        // Base first, in `image.json` order: the order *is* the layer stack.
        MT_EXPECT_EQ(layers[0].as_object().find("digest")->second.as_string(),
                     std::string("sha256:base"));
        // The local delta is named by the digest of the bytes that went out,
        // which the body of request 3 lets us check independently.
        MT_EXPECT_EQ(layers[1].as_object().find("digest")->second.as_string(),
                     core::Sha256Digest(transport.BodyOf(3)));
    }

    // The config descriptor matches the blob that was actually uploaded.
    MT_EXPECT_EQ(fields.find("config")->second.as_object().find("digest")->second.as_string(),
                 core::Sha256Digest(transport.BodyOf(7)));

    // And the tag annotation, which is how a published manifest is traced
    // back to the snapshot it came from.
    const std::string tag = SnapshotTag(FixtureId(), DiskImageSubject::Rootfs());
    const core::JsonObject& annotations = fields.find("annotations")->second.as_object();
    MT_EXPECT_EQ(annotations.find(kSnapshotTagAnnotation)->second.as_string(), tag);
    RemoveTreeBestEffort(dir);
}

/// A taken tag costs one `HEAD`. Discovering it after the blobs were pushed
/// would leave them with no manifest naming them, so the next publish
/// re-uploads them and nothing collects the orphans.
MT_TEST(export_refuses_a_taken_tag_before_uploading_anything) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(core::fs::Write(delta, "delta-bytes").ok());
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(200));
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        // `ManifestExists` maps to InvalidRequest: the caller fixes it by
        // choosing another tag, and a retry of the same one cannot succeed.
        MT_EXPECT_TRUE(out.error().kind == RepositoryErrorKind::InvalidRequest);
    }
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(1));
    RemoveTreeBestEffort(dir);
}

/// A config blob the registry already has is not re-uploaded. The config is
/// content addressed like any other blob, so an unchanged configuration costs
/// one `HEAD` on a re-publish.
MT_TEST(export_skips_a_config_blob_the_registry_already_has) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));  // tag free
    transport.replies.push_back(ScriptedTransport::Status(200));  // config present
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    // No session was opened at all, which is the whole saving.
    MT_EXPECT_EQ(transport.CountOf("POST"), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(3));
    RemoveTreeBestEffort(dir);
}

/// An unmodelled image shape fails before a single request. The plan is what
/// becomes blob uploads and a manifest, so a lower this planner cannot
/// classify must stop the export rather than be approximated.
MT_TEST(export_refuses_an_unmodelled_image_without_contacting_the_registry) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(core::fs::Write(delta, "delta").ok());
    // A remote lower *after* a local delta would put the snapshot's own writes
    // below a registry layer.
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\",\"digest\":\"sha256:d\",\"size\":5},"
                        "{\"digest\":\"sha256:late\",\"size\":7}]");

    ScriptedTransport transport;
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(0));
    RemoveTreeBestEffort(dir);
}

// There is deliberately no test driving `Export`'s `ValidateSnapshotTag`
// refusal: it cannot be reached through this API. A rootfs tag is a fixed
// format over a UUID, and a drive tag is that plus `-drive-`, at most 32
// sanitised characters and 12 hex digits — 106 bytes of always-legal
// characters, which `generated_drive_tags_are_always_valid_tags` above pins.
// The check inside `Export` is defence in depth against a future tag scheme,
// so a test for it here could only assert the branch is unreachable, which is
// what this comment does instead.

/// The local delta goes through `PrepareLayerUpload` with the exporter's
/// `publish_compression`, not straight to the wire.
///
/// Asserted through the zfile probe: in `ZFile` mode a file that is not a
/// layer at all fails the probe, and that failure has to surface instead of
/// the raw bytes being uploaded. An exporter that ignored its
/// `publish_compression` would pass this plain file through and succeed.
MT_TEST(export_routes_a_local_delta_through_publish_compression) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    // Shorter than the 512-byte header slot, so the probe cannot read it.
    MT_EXPECT_TRUE(core::fs::Write(delta, "not-an-overlaybd-layer").ok());
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));  // tag free
    transport.replies.push_back(ScriptedTransport::Status(404));  // delta absent
    AcrDiskImageExporter exporter(BuilderFor(&transport), ZFileMode());

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().ToString().find(delta) != std::string::npos);
    }
    // Nothing was uploaded: the failure came before the session was opened.
    MT_EXPECT_EQ(transport.CountOf("POST"), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(transport.CountOf("PATCH"), static_cast<std::size_t>(0));
    RemoveTreeBestEffort(dir);
}

/// The default is `Raw()` — no compression. A caller that has said nothing
/// about compression must not get it, which is the same deliberate inversion
/// `PublishPolicy()` makes.
MT_TEST(export_defaults_to_uploading_a_local_delta_uncompressed) {
    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(core::fs::Write(delta, "not-an-overlaybd-layer").ok());
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Session("/uploads/l"));
    transport.replies.push_back(ScriptedTransport::Status(202));
    transport.replies.push_back(ScriptedTransport::Status(201));
    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Session("/uploads/c"));
    transport.replies.push_back(ScriptedTransport::Status(201));
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));
    // No compression argument.
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    // The same file that failed the probe under ZFile mode goes out verbatim
    // here, which is what "Raw passes anything through" means.
    if (out.ok()) MT_EXPECT_EQ(transport.BodyOf(3), std::string("not-an-overlaybd-layer"));
    RemoveTreeBestEffort(dir);
}

/// A sparse overlaybd delta is densified before upload, and **every content
/// reference describes the dense bytes**.
///
/// This is the one case where the planner cannot supply a descriptor at all —
/// `DenseOverlaybd` carries none, because hashing the sparse file would record
/// a digest for content the registry never receives. So it is also the case
/// that proves the digests in the manifest come from the upload rather than
/// from the plan: there is nothing in the plan to copy.
MT_TEST(export_densifies_a_sparse_delta_and_names_the_dense_bytes) {
    // Where holes are a filesystem heuristic the layer is not recognised as
    // sparse and the planner falls back to a raw upload, which would make this
    // test assert the wrong branch. Skipped rather than asserted, matching the
    // gate on the other dense-export tests.
    if (!obd::sys::SparseExtentsAreReliable()) {
        MT_EXPECT_TRUE(true);
        return;
    }

    const std::string dir = MakeTempDir();
    const std::string delta = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(BuildSparseLayer(delta));
    const std::string on_disk = ReadWholeFile(delta);
    const std::string image =
        WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123},"
                        "{\"file\":\"" + delta + "\"}]");

    ScriptedTransport transport;
    transport.replies.push_back(ScriptedTransport::Status(404));            // 0 tag free
    transport.replies.push_back(ScriptedTransport::Status(404));            // 1 blob absent
    transport.replies.push_back(ScriptedTransport::Session("/uploads/l"));  // 2 session
    transport.replies.push_back(ScriptedTransport::Status(202));            // 3 PATCH
    transport.replies.push_back(ScriptedTransport::Status(201));            // 4 PUT
    transport.replies.push_back(ScriptedTransport::Status(404));            // 5 config absent
    transport.replies.push_back(ScriptedTransport::Session("/uploads/c"));  // 6 config session
    transport.replies.push_back(ScriptedTransport::Status(201));            // 7 config PUT
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));  // 8 manifest
    AcrDiskImageExporter exporter(BuilderFor(&transport));

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  note: %s\n", out.error().ToString().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_EQ(transport.requests.size(), static_cast<std::size_t>(9));

    // What went out is the *densified layer*, not the file on disk — the
    // holes are materialised into the layer's data area, so this is neither
    // the sparse file nor a raw image of the virtual disk.
    const std::string sent = transport.BodyOf(3);
    MT_EXPECT_TRUE(!sent.empty());
    MT_EXPECT_TRUE(sent != on_disk);

    const std::string dense_digest = core::Sha256Digest(sent);
    // The existence check was aimed at the dense digest, so a blob the
    // registry already holds is still deduplicated correctly.
    MT_EXPECT_EQ(transport.UrlOf(1), std::string(kSourceBlobs) + "/" + dense_digest);
    // The completing PUT claims that digest...
    MT_EXPECT_TRUE(transport.UrlOf(4).find("digest=" + dense_digest) != std::string::npos);

    // ...and so does the manifest. Nothing downstream re-derives it, which is
    // why this is the assertion that matters most in the whole export.
    const core::Optional<core::Json> manifest = ParseJson(transport.BodyOf(8));
    MT_EXPECT_TRUE(manifest.has_value());
    if (manifest.has_value()) {
        const core::JsonArray& layers =
            manifest->as_object().find("layers")->second.as_array();
        MT_EXPECT_EQ(layers.size(), static_cast<std::size_t>(2));
        if (layers.size() == 2) {
            const core::JsonObject& dense = layers[1].as_object();
            MT_EXPECT_EQ(dense.find("digest")->second.as_string(), dense_digest);
            MT_EXPECT_EQ(static_cast<std::size_t>(dense.find("size")->second.as_int()),
                         sent.size());
            // The overlaybd annotations have to agree with the descriptor, or
            // the snapshotter would look for a blob that was never pushed.
            const core::JsonObject& annotations =
                dense.find("annotations")->second.as_object();
            MT_EXPECT_EQ(annotations.find(kOverlaybdBlobDigestAnnotation)->second.as_string(),
                         dense_digest);
        }
    }

    // The committed layer ref, too: this is what the record will carry.
    if (out.value().layers.size() == 2) {
        MT_EXPECT_EQ(out.value().layers[1].external.digest, dense_digest);
        MT_EXPECT_EQ(static_cast<std::size_t>(out.value().layers[1].external.size),
                     sent.size());
    }
    RemoveTreeBestEffort(dir);
}

/// One client per registry, built once. Without the cache every blob would pay
/// a credential load and a 401 round trip.
MT_TEST(export_builds_one_client_for_the_registry) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123}]");

    ScriptedTransport transport;
    int built = 0;
    AcrClient::Transport fn = transport.Fn();
    int* counter = &built;
    AcrDiskImageExporter::ClientBuilder builder =
        [fn, counter](const std::string&) -> core::Expected<AcrClient, AcrClientError> {
        ++*counter;
        DockerRegistryCredentials credentials;
        credentials.username = "alice";
        credentials.password = "pw";
        return AcrClient(core::Optional<DockerRegistryCredentials>(credentials),
                         AcrClientOptions(), fn);
    };

    transport.replies.push_back(ScriptedTransport::Status(404));
    transport.replies.push_back(ScriptedTransport::Status(200));
    transport.replies.push_back(ScriptedTransport::Manifest("sha256:mf"));
    AcrDiskImageExporter exporter(builder);
    MT_EXPECT_TRUE(exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image).ok());
    MT_EXPECT_EQ(built, 1);
    RemoveTreeBestEffort(dir);
}

/// A failure building the client is reported, not swallowed into an export
/// that silently published nothing.
MT_TEST(export_reports_a_client_build_failure) {
    const std::string dir = MakeTempDir();
    const std::string image = WriteImage(dir, "[{\"digest\":\"sha256:base\",\"size\":123}]");

    AcrDiskImageExporter::ClientBuilder builder =
        [](const std::string& registry) -> core::Expected<AcrClient, AcrClientError> {
        return core::make_unexpected(AcrClientError::MissingCredentials(registry));
    };
    AcrDiskImageExporter exporter(builder);

    const RepositoryResult<DiskImageExportOutcome> out =
        exporter.Export(FixtureId(), DiskImageSubject::Rootfs(), image);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        // `MissingCredentials` is a backend failure, not an InvalidRequest: an
        // operator cannot fix it by changing the request.
        MT_EXPECT_TRUE(out.error().kind == RepositoryErrorKind::Backend);
    }
    RemoveTreeBestEffort(dir);
}

MT_MAIN
