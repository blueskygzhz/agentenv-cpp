// SPDX-License-Identifier: MIT
// Rust: the `LayerStore::new` → `prepare_layer_upload` → `put_prepared` chain
// in src/snapshot/store.rs, driven by `repository.publish_policy()`.
//
// These tests close the loop that the unit tests on either side cannot:
// `test_oss_repository.cc` asserts the operator's switch reaches the
// repository's policy, and `test_snapshot_recontainerize.cc` asserts a raw
// layer compresses when a mode says so -- but neither shows that the policy is
// what *selects* the mode on the publish path. A wiring mistake between them
// would leave both green while every published layer went up raw.
//
// The repository here is a recording stub rather than the OSS one, so the
// chain is exercised without libcurl or an HTTP fixture. It captures the blob
// *contents* at `PutBlob` time, which is required rather than tidy: the
// compacted layer is a temporary that is unlinked as soon as the descriptor
// owning it dies, so reading it afterwards would find nothing.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/backends.h"
#include "agentenv/snapshot/repository/interfaces.h"
#include "agentenv/snapshot/store.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/zfile.h"
#include "microtest.h"

using namespace agentenv;                       // NOLINT
using namespace agentenv::snapshot;              // NOLINT
using namespace agentenv::snapshot::repository;  // NOLINT

namespace obd = agentenv::storage::overlaybd;
namespace obdz = agentenv::storage::overlaybd::zfile;

namespace {

const uint64_t kPage = 4096;
const uint64_t kVirtualSize = 3 * kPage;

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/aenv-pubcomp-XXXXXX";
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

/// A repository that records what it was asked to store, and reports whatever
/// publish policy the test gives it.
///
/// Only `PutBlob` and `GetPublishPolicy` are overridden; everything else
/// inherits the base class' `Unsupported` bodies, which is the point -- the
/// publish path under test must not need anything more.
class RecordingRepository : public SnapshotRepository {
 public:
    explicit RecordingRepository(const PublishPolicy& policy) : policy_(policy) {}

    PublishPolicy GetPublishPolicy() const { return policy_; }

    RepositoryResult<core::Unit> PutBlob(const BlobKey& key, const BlobBody& body) {
        (void)key;
        if (body.kind != BlobBody::Kind::File) {
            return core::make_unexpected(
                RepositoryError::Unsupported("this stub only stores file bodies"));
        }
        // Read now, not later: the path is a temporary owned by the
        // `PreparedLayerUpload` that is still alive only for this call.
        const core::Expected<std::string, std::string> bytes =
            core::fs::ReadToString(body.path);
        if (!bytes.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("read stub blob body", bytes.error()));
        }
        stored_paths.push_back(body.path);
        stored_bytes.push_back(bytes.value());
        return core::Unit();
    }

    // Record methods are never reached by the layer-store path.
    RepositoryResult<SnapshotRecord> Create(const SnapshotRecord&) {
        return core::make_unexpected(RepositoryError::Unsupported("Create"));
    }
    RepositoryResult<core::Optional<SnapshotRecord> > Get(const std::string&) {
        return core::make_unexpected(RepositoryError::Unsupported("Get"));
    }
    RepositoryResult<std::vector<SnapshotRecord> > List(const SnapshotListFilter&) {
        return core::make_unexpected(RepositoryError::Unsupported("List"));
    }
    RepositoryResult<core::Unit> Delete(const std::string&) {
        return core::make_unexpected(RepositoryError::Unsupported("Delete"));
    }
    RepositoryResult<core::Optional<core::SnapshotId> > ResolveAlias(const std::string&) {
        return core::make_unexpected(RepositoryError::Unsupported("ResolveAlias"));
    }
    RepositoryResult<SnapshotRecord> TryStartBuild(const core::SnapshotId&) {
        return core::make_unexpected(RepositoryError::Unsupported("TryStartBuild"));
    }
    RepositoryResult<core::Unit> MarkBuildError(const core::SnapshotId&,
                                                 const TemplateBuildErrorReason&) {
        return core::make_unexpected(RepositoryError::Unsupported("MarkBuildError"));
    }

    std::vector<std::string> stored_paths;
    std::vector<std::string> stored_bytes;

 private:
    PublishPolicy policy_;
};

PublishPolicy RawPolicy() { return PublishPolicy(); }

PublishPolicy ZFilePolicy(cfg::OverlaybdCompressionAlgorithm algorithm, std::size_t workers) {
    PublishPolicy policy;
    policy.compression.enabled = true;
    policy.compression.algorithm = algorithm;
    policy.compression.workers = workers;
    return policy;
}

/// A sealed single-file overlaybd layer with three distinctly filled pages.
bool WriteSealedRawLayer(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> data =
        obd::LocalFile::New(path);
    if (!data.ok()) return false;
    const core::Expected<std::shared_ptr<obd::LsmtFile>, std::string> layer =
        obd::LsmtFile::Create(data.value(), std::shared_ptr<obd::VirtualFile>(), kVirtualSize,
                              true);
    if (!layer.ok()) return false;
    const uint8_t fills[3] = {0x11, 0x22, 0x33};
    for (int i = 0; i < 3; ++i) {
        const std::vector<uint8_t> page(static_cast<size_t>(kPage), fills[i]);
        if (!layer.value()->WriteAt(i * kPage, &page[0], page.size()).ok()) return false;
    }
    return layer.value()->CloseSeal().ok();
}

std::string ToStr(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    return std::string(buf);
}

/// An overlaybd image config naming one local lower with its real descriptor.
bool WriteImageConfig(const std::string& config_path, const std::string& layer_path) {
    const core::Expected<core::FileDigest, std::string> described =
        core::DescribeFile(layer_path);
    if (!described.ok()) return false;
    const std::string json = "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + layer_path +
                             "\",\"digest\":\"" + described.value().sha256 + "\",\"size\":" +
                             ToStr(described.value().size) + "}],\"upper\":{},\"resultFile\":\"\"}";
    return core::fs::Write(config_path, json).ok();
}

/// Whether `bytes` begins with a zfile header, judged the same way the runtime
/// judges it -- by writing the bytes out and probing the file.
int ProbeZFileBytes(const TempDir& dir, const std::string& bytes) {
    const std::string probe = dir.At("probe.bin");
    if (!core::fs::Write(probe, bytes).ok()) return -1;
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> f =
        obd::LocalFile::OpenRo(probe);
    if (!f.ok()) return -1;
    const core::Expected<int, std::string> out = obdz::IsZFile(f.value().get());
    core::fs::RemoveFile(probe);
    return out.ok() ? out.value() : -1;
}

}  // namespace

// ---------------------------------------------------------------------------

/// The default policy publishes the source bytes unchanged.
///
/// The descriptor the image config declares is honoured, so the stored blob
/// is byte-identical to the layer on disk -- which is what "raw" has to mean
/// for a publish that trusts a capture-side digest.
MT_TEST(the_default_policy_publishes_a_layer_unchanged) {
    TempDir dir;
    const std::string layer = dir.At("rootfs.commit");
    const std::string config = dir.At("image.json");
    MT_EXPECT_TRUE(WriteSealedRawLayer(layer));
    MT_EXPECT_TRUE(WriteImageConfig(config, layer));

    RecordingRepository repository(RawPolicy());
    const RepositoryResult<std::vector<OverlaybdLayerRef> > stored =
        StoreVolumeLayers(repository, config);
    MT_EXPECT_TRUE(stored.ok());
    if (!stored.ok()) {
        std::fprintf(stderr, "  note: %s\n", stored.error().ToString().c_str());
        return;
    }

    MT_EXPECT_EQ(static_cast<int>(repository.stored_bytes.size()), 1);
    if (repository.stored_bytes.empty()) return;
    // Uploaded in place: the stored path is the source itself.
    MT_EXPECT_EQ(repository.stored_paths[0], layer);
    const core::Expected<std::string, std::string> original = core::fs::ReadToString(layer);
    MT_EXPECT_TRUE(original.ok());
    if (original.ok()) MT_EXPECT_TRUE(repository.stored_bytes[0] == original.value());
    MT_EXPECT_EQ(ProbeZFileBytes(dir, repository.stored_bytes[0]), 0);
}

/// A policy with compression enabled publishes a zfile, and the managed
/// descriptor names the compressed bytes.
///
/// Three things are asserted together because each alone would pass on a
/// broken wiring: the stored blob is a zfile (so compression happened), it is
/// smaller than the source (so it is not a zfile container around raw
/// blocks), and the recorded digest hashes the *stored* bytes rather than the
/// descriptor the image config declared (so the manifest cannot name a blob
/// nobody can verify).
MT_TEST(an_enabled_policy_publishes_a_compressed_layer_described_by_its_own_digest) {
    TempDir dir;
    const std::string layer = dir.At("rootfs.commit");
    const std::string config = dir.At("image.json");
    MT_EXPECT_TRUE(WriteSealedRawLayer(layer));
    MT_EXPECT_TRUE(WriteImageConfig(config, layer));

    const core::Expected<core::FileDigest, std::string> source =
        core::DescribeFile(layer);
    MT_EXPECT_TRUE(source.ok());

    RecordingRepository repository(ZFilePolicy(cfg::OverlaybdCompressionAlgorithm::Lz4, 2));
    const RepositoryResult<std::vector<OverlaybdLayerRef> > stored =
        StoreVolumeLayers(repository, config);
    MT_EXPECT_TRUE(stored.ok());
    if (!stored.ok()) {
        std::fprintf(stderr, "  note: %s\n", stored.error().ToString().c_str());
        return;
    }

    MT_EXPECT_EQ(static_cast<int>(repository.stored_bytes.size()), 1);
    if (repository.stored_bytes.empty() || !source.ok()) return;

    // Compressed, and genuinely so.
    MT_EXPECT_EQ(ProbeZFileBytes(dir, repository.stored_bytes[0]), 1);
    MT_EXPECT_TRUE(repository.stored_bytes[0].size() < source.value().size);
    // Not the source file: a temporary was published instead.
    MT_EXPECT_TRUE(repository.stored_paths[0] != layer);

    // The returned descriptor describes what was stored, not what was read.
    MT_EXPECT_EQ(static_cast<int>(stored.value().size()), 1);
    if (stored.value().empty()) return;
    MT_EXPECT_TRUE(stored.value()[0].is_managed());
    MT_EXPECT_TRUE(stored.value()[0].managed.digest != source.value().sha256);
    MT_EXPECT_TRUE(stored.value()[0].managed.size == repository.stored_bytes[0].size());

    // Hashed here independently, so agreement is not `DescribeFile` agreeing
    // with itself.
    MT_EXPECT_EQ(stored.value()[0].managed.digest,
                 "sha256:" + core::Sha256Hex(repository.stored_bytes[0]));
}

/// Each configured algorithm reaches the compressor.
///
/// Carrying `enabled` across while dropping the algorithm would publish lz4
/// where zstd was configured -- compressed, readable, and wrong. The two
/// outputs are compared to each other because that is the only local evidence
/// the algorithm field was honoured: both are valid zfiles, so only their
/// bytes differ.
MT_TEST(the_configured_algorithm_reaches_the_compressor) {
    TempDir dir;
    const std::string layer = dir.At("rootfs.commit");
    const std::string config = dir.At("image.json");
    MT_EXPECT_TRUE(WriteSealedRawLayer(layer));
    MT_EXPECT_TRUE(WriteImageConfig(config, layer));

    std::string lz4_bytes;
    std::string zstd_bytes;
    {
        RecordingRepository repository(ZFilePolicy(cfg::OverlaybdCompressionAlgorithm::Lz4, 1));
        MT_EXPECT_TRUE(StoreVolumeLayers(repository, config).ok());
        if (!repository.stored_bytes.empty()) lz4_bytes = repository.stored_bytes[0];
    }
    {
        RecordingRepository repository(ZFilePolicy(cfg::OverlaybdCompressionAlgorithm::Zstd, 1));
        MT_EXPECT_TRUE(StoreVolumeLayers(repository, config).ok());
        if (!repository.stored_bytes.empty()) zstd_bytes = repository.stored_bytes[0];
    }

    MT_EXPECT_TRUE(!lz4_bytes.empty() && !zstd_bytes.empty());
    if (lz4_bytes.empty() || zstd_bytes.empty()) return;
    MT_EXPECT_EQ(ProbeZFileBytes(dir, lz4_bytes), 1);
    MT_EXPECT_EQ(ProbeZFileBytes(dir, zstd_bytes), 1);
    MT_EXPECT_TRUE(lz4_bytes != zstd_bytes);
}

/// An already-compressed layer is published unchanged under an enabled
/// policy, so a republish is idempotent rather than doubly compressed.
MT_TEST(an_enabled_policy_leaves_an_already_compressed_layer_alone) {
    TempDir dir;
    const std::string layer = dir.At("rootfs.commit");
    const std::string config = dir.At("image.json");
    MT_EXPECT_TRUE(WriteSealedRawLayer(layer));
    MT_EXPECT_TRUE(WriteImageConfig(config, layer));

    // First publish produces the compressed bytes.
    std::string compressed;
    {
        RecordingRepository repository(ZFilePolicy(cfg::OverlaybdCompressionAlgorithm::Lz4, 1));
        MT_EXPECT_TRUE(StoreVolumeLayers(repository, config).ok());
        if (!repository.stored_bytes.empty()) compressed = repository.stored_bytes[0];
    }
    MT_EXPECT_TRUE(!compressed.empty());
    if (compressed.empty()) return;

    // Feed those bytes back in as the source layer.
    const std::string zfile_layer = dir.At("compressed.commit");
    const std::string zfile_config = dir.At("image-zfile.json");
    MT_EXPECT_TRUE(core::fs::Write(zfile_layer, compressed).ok());
    MT_EXPECT_TRUE(WriteImageConfig(zfile_config, zfile_layer));

    RecordingRepository repository(ZFilePolicy(cfg::OverlaybdCompressionAlgorithm::Lz4, 1));
    MT_EXPECT_TRUE(StoreVolumeLayers(repository, zfile_config).ok());
    MT_EXPECT_EQ(static_cast<int>(repository.stored_bytes.size()), 1);
    if (repository.stored_bytes.empty()) return;
    // Byte-identical, and published in place rather than through a temporary.
    MT_EXPECT_TRUE(repository.stored_bytes[0] == compressed);
    MT_EXPECT_EQ(repository.stored_paths[0], zfile_layer);
}

MT_MAIN
