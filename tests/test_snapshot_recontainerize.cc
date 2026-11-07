// SPDX-License-Identifier: MIT
// Rust: src/snapshot/recontainerize.rs and the `OverlaybdCompactOutput` half
// of src/overlaybd/compact.rs.
//
// The invariant under test is that the returned descriptor describes the bytes
// that will actually be uploaded. Every content reference downstream is
// derived from it — the managed-layer digest, the OSS object key, the ACR blob
// digest, the overlaybd blob annotations in the manifest — so a descriptor
// that described some *other* bytes would produce a manifest the registry
// accepts and no puller can verify.
#include "agentenv/snapshot/recontainerize.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/switch_file.h"
#include "agentenv/storage/overlaybd/tar.h"
#include "agentenv/storage/overlaybd/zfile.h"
#include "microtest.h"

using namespace agentenv;             // NOLINT
using namespace agentenv::snapshot;   // NOLINT

namespace obd = agentenv::storage::overlaybd;
namespace obdz = agentenv::storage::overlaybd::zfile;

namespace {

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_recon_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

overlaybd::OverlaybdCompactOutput ZFileMode() {
    return overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 1);
}

/// A plain file that is not an overlaybd layer at all.
void WritePlain(const std::string& path, const std::string& contents) {
    MT_EXPECT_TRUE(core::fs::Write(path, contents).ok());
}

/// A sealed single-file overlaybd layer holding a recognisable pattern — the
/// raw input the publish path recontainerizes.
///
/// Three pages with distinct fills rather than one: a compaction that lost
/// track of block order would still round-trip a uniform payload. The pattern
/// is deliberately compressible, so the zfile output is smaller than the
/// input and an accidental passthrough would be visible as well as detectable.
bool WriteSealedRawLayer(const std::string& path, std::vector<uint8_t>* contents) {
    const uint64_t kPage = 4096;
    const uint64_t kVirtualSize = 3 * kPage;

    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> data =
        obd::LocalFile::New(path);
    if (!data.ok()) return false;
    const core::Expected<std::shared_ptr<obd::LsmtFile>, std::string> layer =
        obd::LsmtFile::Create(data.value(), std::shared_ptr<obd::VirtualFile>(), kVirtualSize,
                              true);
    if (!layer.ok()) return false;

    std::vector<uint8_t> all;
    const uint8_t fills[3] = {0x11, 0x22, 0x33};
    for (int i = 0; i < 3; ++i) {
        const std::vector<uint8_t> page(static_cast<size_t>(kPage), fills[i]);
        if (!layer.value()->WriteAt(i * kPage, &page[0], page.size()).ok()) return false;
        all.insert(all.end(), page.begin(), page.end());
    }
    if (!layer.value()->CloseSeal().ok()) return false;
    if (contents != 0) *contents = all;
    return true;
}

/// Read `len` bytes back through the same open chain the runtime uses
/// (`LocalFile` → tar adaptor → switch file → LSMT merge view).
///
/// Going through the switch file is the point: that is what transparently
/// decompresses a zfile layer, so this asserts the output is readable *as a
/// layer*, not merely that it parses as a zfile container.
bool ReadSealedLayer(const std::string& path, size_t len, std::vector<uint8_t>* out) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> local =
        obd::LocalFile::OpenRo(path);
    if (!local.ok()) return false;
    const core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> tar_adapted =
        obd::NewTarFileAdaptor(local.value());
    if (!tar_adapted.ok()) return false;
    const core::Expected<std::shared_ptr<obd::SwitchFile>, std::string> switched =
        obd::NewSwitchFile(tar_adapted.value(), true, path.c_str());
    if (!switched.ok()) return false;

    std::vector<std::shared_ptr<obd::VirtualFile> > files;
    files.push_back(std::shared_ptr<obd::VirtualFile>(switched.value()));
    const core::Expected<std::shared_ptr<obd::LsmtReadOnlyFile>, std::string> merged =
        obd::OpenFilesRo(files);
    if (!merged.ok()) return false;

    out->assign(len, 0);
    const core::Expected<size_t, std::string> got =
        merged.value()->ReadAtInto(0, &(*out)[0], len);
    return got.ok() && got.value() == len;
}

/// Synthesize a file whose first 512 bytes are a *valid* zfile header, so
/// `IsZFile` answers 1.
///
/// A header with no body: enough and only enough to drive the "input is
/// already compressed" branch, which is decided by the header alone. The
/// encoder is available now (`WriteSealedRawLayer` + a zfile mode produces a
/// real one), but a probe test should not depend on it -- the branch under
/// test is reached before any body is read.
bool WriteZFileHeaderOnly(const std::string& path) {
    obdz::HeaderTrailer ht;
    ht.SetFlagBit(obdz::HeaderTrailer::kFlagShiftHeader);
    ht.SetFlagBit(obdz::HeaderTrailer::kFlagShiftSealed);
    ht.SetFlagBit(obdz::HeaderTrailer::kFlagShiftCalcDigest);

    std::vector<uint8_t> buf(obdz::kHeaderTrailerSpace, 0);
    // `Encode512` writes the digest field verbatim; it does not compute it.
    // The digest covers the slot with its own four bytes zeroed, so encoding
    // once with `digest = 0` produces exactly the bytes it is taken over.
    ht.digest = 0;
    if (!ht.Encode512(&buf[0], buf.size()).ok()) return false;
    ht.digest = obdz::Crc32c(&buf[0], buf.size());
    if (!ht.Encode512(&buf[0], buf.size()).ok()) return false;
    return core::fs::Write(path, std::string(reinterpret_cast<const char*>(&buf[0]), buf.size()))
        .ok();
}

int ProbeZFile(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> f =
        obd::LocalFile::OpenRo(path);
    if (!f.ok()) return -1;
    const core::Expected<int, std::string> out = obdz::IsZFile(f.value().get());
    return out.ok() ? out.value() : -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// OverlaybdCompactOutput
// ---------------------------------------------------------------------------

MT_TEST(compression_disabled_resolves_to_raw) {
    cfg::SnapshotPublishCompressionConfig config;
    config.enabled = false;
    // Set deliberately: a disabled switch must resolve to Raw regardless of
    // what the other fields say.
    config.algorithm = cfg::OverlaybdCompressionAlgorithm::Zstd;
    config.workers = 8;
    MT_EXPECT_TRUE(
        overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(config).is_raw());
}

MT_TEST(compression_enabled_carries_the_algorithm_through) {
    cfg::SnapshotPublishCompressionConfig config;
    config.enabled = true;
    config.algorithm = cfg::OverlaybdCompressionAlgorithm::Zstd;
    config.workers = 4;
    const overlaybd::OverlaybdCompactOutput mode =
        overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(config);
    MT_EXPECT_TRUE(mode.is_zfile());
    MT_EXPECT_TRUE(mode.algorithm == cfg::OverlaybdCompressionAlgorithm::Zstd);
    MT_EXPECT_EQ(mode.workers, static_cast<std::size_t>(4));
}

/// The worker count is clamped to `[1, 64]`. Both bounds matter: the upper one
/// stops an absurd config from spawning thousands of OS threads per layer, and
/// the lower one stops `workers = 0` — which is not "slower compression" but
/// no progress at all.
MT_TEST(the_worker_count_is_clamped_at_both_ends) {
    cfg::SnapshotPublishCompressionConfig config;
    config.enabled = true;

    config.workers = 0;
    MT_EXPECT_EQ(
        overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(config).workers,
        static_cast<std::size_t>(1));

    config.workers = 100000;
    MT_EXPECT_EQ(
        overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(config).workers,
        overlaybd::kMaxCompressionWorkers);
    MT_EXPECT_EQ(overlaybd::kMaxCompressionWorkers, static_cast<std::size_t>(64));

    // The boundary itself is kept, not clamped away.
    config.workers = 64;
    MT_EXPECT_EQ(
        overlaybd::OverlaybdCompactOutput::FromPublishCompressionConfig(config).workers,
        static_cast<std::size_t>(64));
}

/// `Raw` carries no algorithm or worker count, so a stale pair left by a
/// reassignment must not take part in equality.
MT_TEST(a_raw_mode_ignores_the_unused_zfile_fields) {
    overlaybd::OverlaybdCompactOutput mode =
        overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Zstd, 9);
    mode = overlaybd::OverlaybdCompactOutput::Raw();
    MT_EXPECT_TRUE(mode == overlaybd::OverlaybdCompactOutput::Raw());
    MT_EXPECT_TRUE(mode != ZFileMode());
    // And two zfile modes differing only in worker count are different.
    MT_EXPECT_TRUE(
        overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 1) !=
        overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 2));
}

// ---------------------------------------------------------------------------
// PrepareLayerUpload — Raw mode
// ---------------------------------------------------------------------------

MT_TEST(raw_mode_passes_the_file_through_and_hashes_it) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";
    WritePlain(source, "some layer bytes");

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(source, overlaybd::OverlaybdCompactOutput::Raw());
    MT_EXPECT_TRUE(prepared.ok());
    if (prepared.ok()) {
        MT_EXPECT_EQ(prepared.value().Path(), source);
        const core::Expected<core::FileDigest, std::string> described =
            core::DescribeFile(source);
        MT_EXPECT_TRUE(described.ok());
        MT_EXPECT_EQ(prepared.value().Digest(), described.value().sha256);
        MT_EXPECT_EQ(static_cast<long long>(prepared.value().Size()),
                     static_cast<long long>(described.value().size));
    }
    RemoveTreeBestEffort(dir);
}

/// Raw mode never opens the file as a layer, so a file that is not an
/// overlaybd layer at all still passes through — upstream only probes when
/// compression is on.
MT_TEST(raw_mode_does_not_probe_the_layer_format) {
    const std::string dir = MakeTempDir();
    const std::string bogus = dir + "/snapshot.commit";
    WritePlain(bogus, "not-an-overlaybd-layer");

    MT_EXPECT_TRUE(PrepareLayerUpload(bogus, overlaybd::OverlaybdCompactOutput::Raw()).ok());
    RemoveTreeBestEffort(dir);
}

/// A trusted descriptor is honoured when the bytes are uploaded unchanged, and
/// the file is *not* re-hashed. That is the whole point: re-hashing every
/// layer on every publish would read the entire image.
MT_TEST(a_trusted_descriptor_is_honoured_on_passthrough_without_rehashing) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";
    WritePlain(source, "some layer bytes");

    const repository::RepositoryResult<PreparedLayerUpload> prepared = PrepareLayerUpload(
        source, overlaybd::OverlaybdCompactOutput::Raw(),
        core::Optional<std::string>("sha256:declared"), 4321);
    MT_EXPECT_TRUE(prepared.ok());
    if (prepared.ok()) {
        MT_EXPECT_EQ(prepared.value().Digest(), std::string("sha256:declared"));
        MT_EXPECT_EQ(static_cast<long long>(prepared.value().Size()), 4321LL);
    }
    RemoveTreeBestEffort(dir);
}

/// A missing source is a backend error naming the file. It is not reported as
/// a zero-length layer, which would publish an empty blob under a real digest.
MT_TEST(a_missing_source_fails_as_a_backend_error) {
    const std::string dir = MakeTempDir();
    const repository::RepositoryResult<PreparedLayerUpload> prepared = PrepareLayerUpload(
        dir + "/absent.commit", overlaybd::OverlaybdCompactOutput::Raw());
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) {
        MT_EXPECT_TRUE(prepared.error().kind == repository::RepositoryErrorKind::Backend);
        MT_EXPECT_TRUE(prepared.error().ToString().find("describe managed layer") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// PrepareLayerUpload — ZFile mode
// ---------------------------------------------------------------------------

/// An already-compressed input is uploaded unchanged, so re-publishing does
/// not recompress and a trusted descriptor still describes the physical bytes.
MT_TEST(zfile_mode_passes_an_already_compressed_input_through) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(WriteZFileHeaderOnly(source));
    // Guard the fixture: if this were not recognised as a zfile the test below
    // would be asserting the raw-input branch instead.
    MT_EXPECT_EQ(ProbeZFile(source), 1);

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(source, ZFileMode());
    MT_EXPECT_TRUE(prepared.ok());
    if (prepared.ok()) {
        MT_EXPECT_EQ(prepared.value().Path(), source);
        const core::Expected<core::FileDigest, std::string> described =
            core::DescribeFile(source);
        MT_EXPECT_EQ(prepared.value().Digest(), described.value().sha256);
    }

    // And with a trusted descriptor, it is kept — the bytes did not change.
    const repository::RepositoryResult<PreparedLayerUpload> trusted = PrepareLayerUpload(
        source, ZFileMode(), core::Optional<std::string>("sha256:declared"), 512);
    MT_EXPECT_TRUE(trusted.ok());
    if (trusted.ok()) {
        MT_EXPECT_EQ(trusted.value().Digest(), std::string("sha256:declared"));
    }
    RemoveTreeBestEffort(dir);
}

/// A corrupt input fails the probe rather than being uploaded. `IsZFile`
/// reports a readable-but-invalid header as an error, not as "not a zfile",
/// and that distinction has to survive to here.
MT_TEST(zfile_mode_fails_the_probe_on_a_truncated_input_instead_of_uploading) {
    const std::string dir = MakeTempDir();
    const std::string tiny = dir + "/snapshot.commit";
    // Shorter than the 512-byte header slot, so the probe cannot even read it.
    WritePlain(tiny, "not-an-overlaybd-layer");

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(tiny, ZFileMode());
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) {
        MT_EXPECT_TRUE(prepared.error().kind == repository::RepositoryErrorKind::Backend);
        MT_EXPECT_TRUE(prepared.error().ToString().find(tiny) != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(zfile_mode_names_the_file_when_it_cannot_be_opened) {
    const std::string dir = MakeTempDir();
    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(dir + "/absent.commit", ZFileMode());
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) {
        MT_EXPECT_TRUE(prepared.error().ToString().find("for zfile probe") !=
                       std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

/// A raw layer is recontainerized into a real zfile, and the descriptor
/// describes the *compressed* bytes.
///
/// This is the test that used to assert the opposite -- a refusal, because
/// nothing could route a compaction into the zfile compressor. Both halves of
/// that gap are now closed (`CompactWriterSink` joins `lsmt::CompactSink` to
/// `util::CompactWriter`, and `CompactLayers` drives it), so the behaviour
/// asserted here is the round-trip rather than the refusal.
///
/// The descriptor is the point. Everything downstream derives its content
/// reference from it, so it is checked against a fresh hash of the file on
/// disk -- not against another call into the same code -- and the uploaded
/// path is checked to be the compacted output rather than the raw source.
MT_TEST(zfile_mode_recontainerizes_a_raw_layer_and_describes_the_compressed_bytes) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";

    std::vector<uint8_t> written;
    MT_EXPECT_TRUE(WriteSealedRawLayer(source, &written));
    // A sealed LSMT layer is not a zfile, which is what puts it on the
    // raw-layer branch.
    MT_EXPECT_EQ(ProbeZFile(source), 0);

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(source, ZFileMode());
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) {
        std::fprintf(stderr, "  note: %s\n", prepared.error().ToString().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    // The upload reads from the compacted temp file, not from the source.
    MT_EXPECT_TRUE(prepared.value().Path() != source);
    MT_EXPECT_EQ(ProbeZFile(prepared.value().Path()), 1);

    // The descriptor describes the bytes on disk. Hashed independently here,
    // so agreement is not just `DescribeFile` agreeing with itself.
    const core::Expected<core::FileDigest, std::string> actual =
        core::DescribeFile(prepared.value().Path());
    MT_EXPECT_TRUE(actual.ok());
    if (actual.ok()) {
        MT_EXPECT_EQ(prepared.value().Digest(), actual.value().sha256);
        MT_EXPECT_TRUE(prepared.value().Size() == actual.value().size);
    }

    // The compressed layer still reads back as the original image. Without
    // this the test would pass on a zfile that merely *parses*.
    std::vector<uint8_t> read_back;
    MT_EXPECT_TRUE(ReadSealedLayer(prepared.value().Path(), written.size(), &read_back));
    MT_EXPECT_TRUE(read_back == written);

    RemoveTreeBestEffort(dir);
}

/// The compacted temp file is unlinked once the descriptor that owns it dies.
///
/// Worth asserting separately: the upload path holds a `PreparedLayerUpload`
/// for the duration of the transfer and nothing else references the file, so
/// a lifetime that ended too early would make the upload read a deleted path,
/// and one that never ended would grow the staging directory by a full layer
/// per publish.
MT_TEST(a_recontainerized_layer_is_unlinked_with_the_descriptor_that_owns_it) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(WriteSealedRawLayer(source, 0));

    std::string temp_path;
    {
        const repository::RepositoryResult<PreparedLayerUpload> prepared =
            PrepareLayerUpload(source, ZFileMode());
        MT_EXPECT_TRUE(prepared.ok());
        if (!prepared.ok()) {
            RemoveTreeBestEffort(dir);
            return;
        }
        temp_path = prepared.value().Path();
        MT_EXPECT_TRUE(core::fs::Exists(temp_path));
    }
    MT_EXPECT_TRUE(!core::fs::Exists(temp_path));
    // The source is untouched by the recontainerization.
    MT_EXPECT_TRUE(core::fs::Exists(source));
    RemoveTreeBestEffort(dir);
}

/// Raw mode leaves the very same input alone, which is what makes the
/// compression above attributable to the mode rather than to the input.
MT_TEST(raw_mode_uploads_a_raw_layer_in_place) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/snapshot.commit";
    MT_EXPECT_TRUE(WriteSealedRawLayer(source, 0));

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(source, overlaybd::OverlaybdCompactOutput::Raw());
    MT_EXPECT_TRUE(prepared.ok());
    if (prepared.ok()) {
        MT_EXPECT_EQ(prepared.value().Path(), source);
        MT_EXPECT_EQ(ProbeZFile(prepared.value().Path()), 0);
    }
    RemoveTreeBestEffort(dir);
}

/// Each algorithm and a fan-out worker count all produce a readable zfile
/// carrying the original image.
///
/// zstd and the multi-worker path are separate code inside the compressor
/// (`CompressBatch` plus the worker pool), and this is the only place the
/// publish path exercises them.
MT_TEST(every_zfile_algorithm_and_worker_count_round_trips_through_the_publish_path) {
    const std::string dir = MakeTempDir();

    struct Case {
        const char* name;
        cfg::OverlaybdCompressionAlgorithm algo;
        std::size_t workers;
    };
    const Case cases[] = {
        {"lz4-1", cfg::OverlaybdCompressionAlgorithm::Lz4, 1},
        {"lz4-4", cfg::OverlaybdCompressionAlgorithm::Lz4, 4},
        {"zstd-1", cfg::OverlaybdCompressionAlgorithm::Zstd, 1},
        {"zstd-4", cfg::OverlaybdCompressionAlgorithm::Zstd, 4},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string source = dir + "/" + cases[i].name + ".commit";
        std::vector<uint8_t> written;
        MT_EXPECT_TRUE(WriteSealedRawLayer(source, &written));

        const repository::RepositoryResult<PreparedLayerUpload> prepared = PrepareLayerUpload(
            source, overlaybd::OverlaybdCompactOutput::ZFile(cases[i].algo, cases[i].workers));
        MT_EXPECT_TRUE(prepared.ok());
        if (!prepared.ok()) {
            std::fprintf(stderr, "  note: %s: %s\n", cases[i].name,
                         prepared.error().ToString().c_str());
            continue;
        }
        MT_EXPECT_EQ(ProbeZFile(prepared.value().Path()), 1);

        std::vector<uint8_t> read_back;
        MT_EXPECT_TRUE(ReadSealedLayer(prepared.value().Path(), written.size(), &read_back));
        MT_EXPECT_TRUE(read_back == written);
    }
    RemoveTreeBestEffort(dir);
}

/// An input that is not an overlaybd layer at all fails, rather than being
/// published as a zfile wrapping garbage.
///
/// The raw-layer branch opens its input through the LSMT merge view, so a
/// plain file has no index to merge and the compaction stops there. Asserted
/// because the previous behaviour -- a blanket "not ported" refusal -- would
/// have masked it.
MT_TEST(zfile_mode_rejects_an_input_that_is_not_an_overlaybd_layer) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/plain.commit";
    // Decodes as 512 bytes but fails `VerifyMagic`, so the probe answers 0
    // and the raw-layer branch is the one taken.
    WritePlain(source, std::string(8192, '\0'));
    MT_EXPECT_EQ(ProbeZFile(source), 0);

    const repository::RepositoryResult<PreparedLayerUpload> prepared =
        PrepareLayerUpload(source, ZFileMode());
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) {
        MT_EXPECT_TRUE(prepared.error().ToString().find("recontainerize layer") !=
                       std::string::npos);
    }
    // Whether the compaction leaves its sibling temp file behind is asserted
    // in the `CompactLayers` tests, where the output directory is this test's
    // to inspect; here the staging file lives under TMPDIR.
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// OwnedTempPath / RetainSource
// ---------------------------------------------------------------------------

MT_TEST(an_owned_temp_path_unlinks_the_file_when_the_last_copy_dies) {
    std::string path;
    {
        const core::Expected<OwnedTempPath, std::string> temp =
            OwnedTempPath::Create("recon-test-");
        MT_EXPECT_TRUE(temp.ok());
        path = temp.value().path();
        MT_EXPECT_TRUE(core::fs::Exists(path));
        {
            // A second reference keeps it alive...
            const OwnedTempPath copy = temp.value();
            MT_EXPECT_TRUE(core::fs::Exists(copy.path()));
        }
        // ...and dropping only that copy must not remove it.
        MT_EXPECT_TRUE(core::fs::Exists(path));
    }
    MT_EXPECT_TRUE(!core::fs::Exists(path));
}

MT_TEST(a_default_owned_temp_path_is_empty_and_harmless) {
    const OwnedTempPath none;
    MT_EXPECT_TRUE(!none.valid());
    MT_EXPECT_TRUE(none.path().empty());
}

/// `RetainSource` keeps a caller-owned temporary alive — the dense-export
/// case, where the file handed to `PrepareLayerUpload` is itself a temporary
/// nobody else holds.
///
/// Every *other* handle is dropped before the assertion, so the upload holds
/// the only reference. Without that, the file would survive regardless and
/// the test would pass even if `RetainSource` did nothing.
MT_TEST(a_retained_source_is_unlinked_only_when_the_prepared_upload_dies) {
    std::string source_path;
    PreparedLayerUpload* upload = NULL;
    {
        const core::Expected<OwnedTempPath, std::string> source =
            OwnedTempPath::Create("recon-src-");
        MT_EXPECT_TRUE(source.ok());
        source_path = source.value().path();
        WritePlain(source_path, "dense-exported bytes");

        const repository::RepositoryResult<PreparedLayerUpload> prepared =
            PrepareLayerUpload(source_path, overlaybd::OverlaybdCompactOutput::Raw());
        MT_EXPECT_TRUE(prepared.ok());
        upload = new PreparedLayerUpload(prepared.value());
        upload->RetainSource(source.value());
        // Passthrough, so the upload path *is* the source path.
        MT_EXPECT_EQ(upload->Path(), source_path);
    }

    // `source` is gone; only the retained handle inside `upload` remains.
    MT_EXPECT_TRUE(core::fs::Exists(source_path));
    delete upload;
    MT_EXPECT_TRUE(!core::fs::Exists(source_path));
}

/// An existing temporary is **not** replaced. Overwriting it would drop the
/// last reference to the file at `Path()` and unlink the very bytes that are
/// about to be uploaded.
///
/// The two outcomes are observably inverted, which is what makes this a real
/// test: if the second call displaced the first, `first_path` would be
/// unlinked at scope exit and `second_path` would survive.
MT_TEST(retain_source_does_not_displace_a_temporary_already_held) {
    std::string first_path;
    std::string second_path;
    PreparedLayerUpload* upload = NULL;
    {
        const core::Expected<OwnedTempPath, std::string> first =
            OwnedTempPath::Create("recon-first-");
        const core::Expected<OwnedTempPath, std::string> second =
            OwnedTempPath::Create("recon-second-");
        MT_EXPECT_TRUE(first.ok() && second.ok());
        first_path = first.value().path();
        second_path = second.value().path();

        upload = new PreparedLayerUpload(
            PreparedLayerUpload::Make(first_path, "sha256:x", 1, first.value()));
        // Must be ignored: the upload already holds a temporary.
        upload->RetainSource(second.value());
    }

    // The first is still held by the upload...
    MT_EXPECT_TRUE(core::fs::Exists(first_path));
    // ...and the second was never adopted, so it is already gone.
    MT_EXPECT_TRUE(!core::fs::Exists(second_path));

    delete upload;
    MT_EXPECT_TRUE(!core::fs::Exists(first_path));
}

MT_MAIN
