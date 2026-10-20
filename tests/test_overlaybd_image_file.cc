// SPDX-License-Identifier: MIT
// Tests for `image_file` / `image_service` — the image-open path.
#include "microtest.h"

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/overlaybd/image_helper.h"
#include "agentenv/storage/overlaybd/image_service.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"

using namespace agentenv;
using namespace agentenv::storage::overlaybd;

namespace {

const uint64_t kBlk = 4096;
const uint64_t kVirtualSize = 64 * kBlk;

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_img_XXXXXX";
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

std::vector<uint8_t> Pattern(size_t len, uint8_t seed) {
    std::vector<uint8_t> out(len, 0);
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(seed + (i % 251));
    return out;
}

/// A real sealed LSMT layer on disk, with `blocks` carrying `seed`-derived
/// bytes. Returns the layer's path.
std::string WriteSealedLayer(const std::string& dir, const std::string& name,
                             const std::string& uuid, const std::vector<uint64_t>& blocks,
                             uint8_t seed) {
    const std::string data_path = core::fs::Join(dir, name);
    const std::string index_path = data_path + ".idx";

    core::Expected<std::shared_ptr<LocalFile>, std::string> data = LocalFile::New(data_path);
    core::Expected<std::shared_ptr<LocalFile>, std::string> index = LocalFile::New(index_path);
    if (!data.ok() || !index.ok()) {
        std::fprintf(stderr, "  FAIL: create layer files\n");
        std::exit(1);
    }

    core::Expected<std::shared_ptr<LsmtFile>, std::string> f = LsmtFile::CreateWithMetadata(
        data.value(), index.value(), kVirtualSize, kRwLogStructured, uuid, std::string(), 0, 0);
    if (!f.ok()) {
        std::fprintf(stderr, "  FAIL: create sealed layer: %s\n", f.error().c_str());
        std::exit(1);
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::vector<uint8_t> d =
            Pattern(static_cast<size_t>(kBlk), static_cast<uint8_t>(seed + blocks[i]));
        if (!f.value()->WriteAt(blocks[i] * kBlk, &d[0], d.size()).ok()) {
            std::fprintf(stderr, "  FAIL: write layer block\n");
            std::exit(1);
        }
    }
    if (!f.value()->CloseSeal().ok()) {
        std::fprintf(stderr, "  FAIL: seal layer\n");
        std::exit(1);
    }
    core::fs::RemoveFile(index_path);
    return data_path;
}

GlobalConfig TestGlobalConfig(const std::string& dir) {
    GlobalConfig cfg;
    cfg.cache_config.cache_type = "file";
    cfg.cache_config.cache_dir = core::fs::Join(dir, "cache");
    cfg.cache_config.cache_size_gb = 1;
    cfg.cache_config.refill_size = 65536;
    return cfg;
}

/// An image config with `layer_paths` as local lowers, bottom to top.
ImageConfig LocalLowerImageConfig(const std::vector<std::string>& layer_paths) {
    ImageConfig cfg;
    for (size_t i = 0; i < layer_paths.size(); ++i) {
        LayerConfig layer;
        layer.file = layer_paths[i];
        cfg.lowers.push_back(layer);
    }
    return cfg;
}

/// A writable upper, pre-sized for `kVirtualSize`.
void AddUpper(ImageConfig* cfg, const std::string& dir) {
    const std::string data_path = core::fs::Join(dir, "upper.data");
    const std::string index_path = core::fs::Join(dir, "upper.index");
    if (!PrepareRuntimeUpper(data_path, core::Optional<std::string>(index_path), kVirtualSize,
                             UpperMode::LogStructured)
             .ok()) {
        std::fprintf(stderr, "  FAIL: prepare upper\n");
        std::exit(1);
    }
    cfg->upper.has_mode = true;
    cfg->upper.mode = UpperMode::LogStructured;
    cfg->upper.data = data_path;
    cfg->upper.index = index_path;
}

ImageService MakeService(const std::string& dir) {
    core::Expected<ImageService, std::string> service = ImageService::New(TestGlobalConfig(dir));
    if (!service.ok()) {
        std::fprintf(stderr, "  FAIL: build service: %s\n", service.error().c_str());
        std::exit(1);
    }
    return service.value();
}

bool BlockEquals(const ByteBuffer& got, uint8_t seed) {
    const std::vector<uint8_t> want = Pattern(static_cast<size_t>(kBlk), seed);
    if (got.size() != want.size()) return false;
    return std::memcmp(&got[0], &want[0], want.size()) == 0;
}


/// A read workload used to compare concurrent against serialised execution.
///
/// The overlap cannot be sampled from inside `ImageFile::ReadAt` -- there is
/// no seam between the guard and the layer stack -- so the shared guard is
/// observed through its only external effect: elapsed time. Each run carries
/// its own serialised baseline, so the assertion is a ratio rather than an
/// absolute duration.
struct ReadWorkerArgs {
    ImageFile* image;
    /// Non-null for the serialised run, which takes it around every read.
    pthread_mutex_t* serialise;
    uint64_t offset;
    size_t iterations;
    size_t bytes;
    size_t failures;
};

void* ReadWorker(void* raw) {
    ReadWorkerArgs* args = static_cast<ReadWorkerArgs*>(raw);
    for (size_t i = 0; i < args->iterations; ++i) {
        if (args->serialise != 0) ::pthread_mutex_lock(args->serialise);
        const core::Expected<ByteBuffer, std::string> got =
            args->image->ReadAt(args->offset, args->bytes);
        if (args->serialise != 0) ::pthread_mutex_unlock(args->serialise);
        if (!got.ok() || got.value().size() != args->bytes) ++args->failures;
    }
    return 0;
}

uint64_t NowMillis() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

/// Runs `kReadThreads` readers, optionally serialised, and returns the
/// wall-clock milliseconds plus the failure count.
uint64_t TimeReads(ImageFile* image, bool serialise, size_t iterations, size_t bytes,
                   size_t* failures) {
    const size_t kReadThreads = 4;
    pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
    std::vector<pthread_t> threads(kReadThreads);
    std::vector<ReadWorkerArgs> args(kReadThreads);

    const uint64_t start = NowMillis();
    for (size_t i = 0; i < kReadThreads; ++i) {
        args[i].image = image;
        args[i].serialise = serialise ? &gate : 0;
        args[i].offset = 0;
        args[i].iterations = iterations;
        args[i].bytes = bytes;
        args[i].failures = 0;
        if (::pthread_create(&threads[i], 0, ReadWorker, &args[i]) != 0) {
            std::fprintf(stderr, "  FAIL: pthread_create\n");
            std::exit(1);
        }
    }
    *failures = 0;
    for (size_t i = 0; i < kReadThreads; ++i) {
        ::pthread_join(threads[i], 0);
        *failures += args[i].failures;
    }
    return NowMillis() - start;
}

}  // namespace

// ============================================================
// opening
// ============================================================

MT_TEST(an_image_of_local_lowers_opens_read_only) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> layers;
    std::vector<uint64_t> b0;
    b0.push_back(0);
    b0.push_back(1);
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", b0, 0x10));

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    // No upper means read-only, and writes must be refused rather than
    // silently dropped.
    MT_EXPECT_TRUE(image.value()->IsReadOnly());
    const std::vector<uint8_t> payload(16, 0xAB);
    MT_EXPECT_TRUE(!image.value()->WriteAt(0, &payload[0], payload.size()).ok());

    MT_EXPECT_EQ(static_cast<long long>(image.value()->SizeBytes()),
                 static_cast<long long>(kVirtualSize));
    // `num_lbas` is derived from the 512-byte block size, not the 4 KiB one.
    MT_EXPECT_EQ(static_cast<long long>(image.value()->NumLbas()),
                 static_cast<long long>(kVirtualSize / kImageDefaultBlockSize));
    MT_EXPECT_EQ(static_cast<long long>(image.value()->BlockSize()),
                 static_cast<long long>(512));

    const core::Expected<ByteBuffer, std::string> got =
        image.value()->ReadAt(0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) MT_EXPECT_TRUE(BlockEquals(got.value(), 0x10));

    RemoveTreeBestEffort(dir);
}

MT_TEST(an_image_with_an_upper_is_writable_and_shadows_the_lower) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> layers;
    std::vector<uint64_t> b0;
    b0.push_back(0);
    b0.push_back(1);
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", b0, 0x10));

    ImageConfig cfg = LocalLowerImageConfig(layers);
    AddUpper(&cfg, dir);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(!image.value()->IsReadOnly());

    // Block 0 exists in the lower; writing it through the upper must win.
    const std::vector<uint8_t> fresh = Pattern(static_cast<size_t>(kBlk), 0x77);
    const core::Expected<size_t, std::string> written =
        image.value()->WriteAt(0, &fresh[0], fresh.size());
    MT_EXPECT_TRUE(written.ok());

    const core::Expected<ByteBuffer, std::string> got =
        image.value()->ReadAt(0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) {
        MT_EXPECT_TRUE(BlockEquals(got.value(), 0x77));
        // Specifically not the lower's bytes.
        MT_EXPECT_TRUE(!BlockEquals(got.value(), 0x10));
    }

    // Block 1 was never written through the upper, so it still reads from the
    // lower -- otherwise the test above would also pass for an image that had
    // simply lost its lower.
    const core::Expected<ByteBuffer, std::string> untouched =
        image.value()->ReadAt(kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(untouched.ok());
    if (untouched.ok()) MT_EXPECT_TRUE(BlockEquals(untouched.value(), 0x11));

    RemoveTreeBestEffort(dir);
}

MT_TEST(stacking_order_puts_the_last_lower_on_top) {
    const std::string dir = MakeTempDir();
    std::vector<uint64_t> block0;
    block0.push_back(0);

    // Both layers own block 0. `lowers` is bottom-to-top, so l1 must win.
    std::vector<std::string> layers;
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", block0, 0x10));
    layers.push_back(WriteSealedLayer(dir, "l1.lsmt", "layer-1-uuid", block0, 0x50));

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<ByteBuffer, std::string> got =
        image.value()->ReadAt(0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) {
        MT_EXPECT_TRUE(BlockEquals(got.value(), 0x50));
        MT_EXPECT_TRUE(!BlockEquals(got.value(), 0x10));
    }

    RemoveTreeBestEffort(dir);
}

MT_TEST(an_image_with_neither_lower_nor_upper_is_refused) {
    const std::string dir = MakeTempDir();
    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(ImageConfig(), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(!image.ok());
    if (!image.ok()) {
        MT_EXPECT_TRUE(image.error().find("no lower and no upper") != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_remote_lower_without_a_repo_blob_url_is_refused) {
    const std::string dir = MakeTempDir();
    ImageConfig cfg;
    LayerConfig layer;
    // No `file`, no `dir`: this is a remote layer, and it names a digest but
    // the config gives nowhere to fetch it from.
    layer.digest = "sha256:" + std::string(64, 'a');
    layer.size = kBlk;
    cfg.lowers.push_back(layer);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(!image.ok());
    if (!image.ok()) {
        MT_EXPECT_TRUE(image.error().find("repoBlobUrl is empty") != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_lower_that_is_neither_local_nor_digested_is_refused) {
    const std::string dir = MakeTempDir();
    ImageConfig cfg;
    cfg.repo_blob_url = "https://example.invalid/v2/repo/blobs";
    cfg.lowers.push_back(LayerConfig());

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(!image.ok());
    if (!image.ok()) {
        MT_EXPECT_TRUE(image.error().find("no local file and no digest") != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(an_unmigrated_lower_shape_is_refused_rather_than_ignored) {
    // warp/targetDigest/gzipIndex lowers would otherwise open as if they were
    // plain layers and hand back the wrong bytes.
    const std::string dir = MakeTempDir();
    std::vector<uint64_t> b0;
    b0.push_back(0);
    const std::string path = WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", b0, 0x10);

    ImageConfig cfg;
    LayerConfig layer;
    layer.file = path;
    layer.gzip_index = core::fs::Join(dir, "gzip.idx");
    cfg.lowers.push_back(layer);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(!image.ok());
    if (!image.ok()) {
        MT_EXPECT_TRUE(image.error().find("not migrated") != std::string::npos);
    }
    RemoveTreeBestEffort(dir);
}

// ============================================================
// sealing
// ============================================================

MT_TEST(sealing_a_read_only_image_is_refused) {
    const std::string dir = MakeTempDir();
    std::vector<uint64_t> b0;
    b0.push_back(0);
    std::vector<std::string> layers;
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", b0, 0x10));

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<core::Optional<LayerDescriptor>, std::string> sealed =
        image.value()->CloseSeal(core::fs::Join(dir, "out.lsmt"));
    MT_EXPECT_TRUE(!sealed.ok());
    if (!sealed.ok()) {
        MT_EXPECT_TRUE(sealed.error().find("requires a writable upper") != std::string::npos);
    }
    // And nothing was produced.
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(dir, "out.lsmt")));

    RemoveTreeBestEffort(dir);
}

MT_TEST(close_seal_moves_the_upper_to_the_output_path) {
    const std::string dir = MakeTempDir();
    ImageConfig cfg;
    AddUpper(&cfg, dir);
    const std::string upper_data = cfg.upper.data;

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    const std::vector<uint8_t> payload = Pattern(static_cast<size_t>(kBlk), 0x33);
    MT_EXPECT_TRUE(image.value()->WriteAt(0, &payload[0], payload.size()).ok());

    // A nested directory, so the "create the parent first" step is exercised.
    const std::string out = core::fs::Join(core::fs::Join(dir, "sealed"), "out.lsmt");
    const core::Expected<core::Optional<LayerDescriptor>, std::string> sealed =
        image.value()->CloseSeal(out);
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) std::fprintf(stderr, "  seal: %s\n", sealed.error().c_str());

    // The move is the observable part: the layer is at the output path and
    // the upper's own path no longer holds it.
    MT_EXPECT_TRUE(core::fs::Exists(out));
    MT_EXPECT_TRUE(!core::fs::Exists(upper_data));

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_sealed_layer_reopens_as_a_lower_carrying_its_bytes) {
    // The round trip that makes `CloseSeal` worth anything: what it produces
    // has to be openable as a lower, with the bytes that were written.
    const std::string dir = MakeTempDir();
    ImageConfig cfg;
    AddUpper(&cfg, dir);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const std::vector<uint8_t> payload = Pattern(static_cast<size_t>(kBlk), 0x44);
    MT_EXPECT_TRUE(image.value()->WriteAt(2 * kBlk, &payload[0], payload.size()).ok());

    const std::string out = core::fs::Join(dir, "sealed.lsmt");
    MT_EXPECT_TRUE(image.value()->CloseSeal(out).ok());

    std::vector<std::string> layers;
    layers.push_back(out);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> reopened =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(reopened.ok());
    if (!reopened.ok()) {
        std::fprintf(stderr, "  reopen: %s\n", reopened.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<ByteBuffer, std::string> got =
        reopened.value()->ReadAt(2 * kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) MT_EXPECT_TRUE(BlockEquals(got.value(), 0x44));

    RemoveTreeBestEffort(dir);
}

MT_TEST(restack_keeps_the_image_serving_and_records_the_snapshot) {
    const std::string dir = MakeTempDir();
    ImageConfig cfg;
    AddUpper(&cfg, dir);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    const std::vector<uint8_t> before = Pattern(static_cast<size_t>(kBlk), 0x55);
    MT_EXPECT_TRUE(image.value()->WriteAt(0, &before[0], before.size()).ok());

    const std::string snapshot = core::fs::Join(dir, "snap.lsmt");
    const core::Expected<core::Optional<LayerDescriptor>, std::string> restacked =
        image.value()->CreateSnapshotAndRestack(snapshot);
    MT_EXPECT_TRUE(restacked.ok());
    if (!restacked.ok()) {
        std::fprintf(stderr, "  restack: %s\n", restacked.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(core::fs::Exists(snapshot));

    // Checked here, before any write: a fresh upper is created at size 0 and
    // must inherit the image's virtual size during the restack itself.
    // `WriteAt` would recompute this from the base and paper over a restack
    // that failed to carry it over -- mutation-checked, this assertion is
    // vacuous if it runs after the write below.
    MT_EXPECT_EQ(static_cast<long long>(image.value()->SizeBytes()),
                 static_cast<long long>(kVirtualSize));
    MT_EXPECT_EQ(static_cast<long long>(image.value()->NumLbas()),
                 static_cast<long long>(kVirtualSize / kImageDefaultBlockSize));

    // Unlike `CloseSeal`, the image stays usable: the pre-snapshot byte still
    // reads through the new lower...
    MT_EXPECT_TRUE(!image.value()->IsReadOnly());
    const core::Expected<ByteBuffer, std::string> old_bytes =
        image.value()->ReadAt(0, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(old_bytes.ok());
    if (old_bytes.ok()) MT_EXPECT_TRUE(BlockEquals(old_bytes.value(), 0x55));

    // ...and the fresh upper still takes writes.
    const std::vector<uint8_t> after = Pattern(static_cast<size_t>(kBlk), 0x66);
    MT_EXPECT_TRUE(image.value()->WriteAt(kBlk, &after[0], after.size()).ok());
    const core::Expected<ByteBuffer, std::string> new_bytes =
        image.value()->ReadAt(kBlk, static_cast<size_t>(kBlk));
    MT_EXPECT_TRUE(new_bytes.ok());
    if (new_bytes.ok()) MT_EXPECT_TRUE(BlockEquals(new_bytes.value(), 0x66));

    RemoveTreeBestEffort(dir);
}

// ============================================================
// service surface
// ============================================================

MT_TEST(oss_urls_are_recognised_by_scheme) {
    MT_EXPECT_TRUE(ImageService::IsOssUrl("oss://bucket/key"));
    MT_EXPECT_TRUE(ImageService::IsOssUrl("s3://bucket/key"));
    MT_EXPECT_TRUE(!ImageService::IsOssUrl("https://example.com/v2/blobs/sha256:ab"));
    MT_EXPECT_TRUE(!ImageService::IsOssUrl("/local/path"));
    MT_EXPECT_TRUE(!ImageService::IsOssUrl(""));
}

MT_TEST(the_p2p_uuid_facade_is_derived_only_from_a_p2p_http_address) {
    const std::string dir = MakeTempDir();

    GlobalConfig disabled = TestGlobalConfig(dir);
    disabled.p2p_config.enable = false;
    disabled.p2p_config.address = "http://127.0.0.1:19145/p2p-http";
    core::Expected<ImageService, std::string> off = ImageService::New(disabled);
    MT_EXPECT_TRUE(off.ok());
    if (off.ok()) MT_EXPECT_TRUE(!off.value().P2pUuidAddress().has_value());

    GlobalConfig enabled = TestGlobalConfig(dir);
    enabled.p2p_config.enable = true;
    enabled.p2p_config.address = "http://127.0.0.1:19145/p2p-http";
    core::Expected<ImageService, std::string> on = ImageService::New(enabled);
    MT_EXPECT_TRUE(on.ok());
    if (on.ok()) {
        const core::Optional<std::string> facade = on.value().P2pUuidAddress();
        MT_EXPECT_TRUE(facade.has_value());
        if (facade.has_value()) {
            MT_EXPECT_TRUE(*facade == std::string("http://127.0.0.1:19145/p2p-uuid"));
        }
    }

    // An address that is not the `/p2p-http` facade yields nothing rather
    // than a guessed URL.
    GlobalConfig other = TestGlobalConfig(dir);
    other.p2p_config.enable = true;
    other.p2p_config.address = "http://127.0.0.1:19145/something-else";
    core::Expected<ImageService, std::string> odd = ImageService::New(other);
    MT_EXPECT_TRUE(odd.ok());
    if (odd.ok()) MT_EXPECT_TRUE(!odd.value().P2pUuidAddress().has_value());

    RemoveTreeBestEffort(dir);
}

MT_TEST(an_unreachable_accelerator_turns_acceleration_off) {
    const std::string dir = MakeTempDir();
    GlobalConfig cfg = TestGlobalConfig(dir);
    cfg.p2p_config.enable = true;
    // Loopback with nothing listening, deliberately not a TEST-NET address:
    // this sandbox routes those through a transparent proxy that accepts
    // every connection, so `192.0.2.1` reports *reachable* here. Loopback is
    // refused by the kernel itself and cannot be intercepted.
    cfg.p2p_config.address = "http://127.0.0.1:1/p2p-http";

    core::Expected<ImageService, std::string> service = ImageService::New(cfg);
    MT_EXPECT_TRUE(service.ok());
    if (service.ok()) MT_EXPECT_TRUE(!service.value().EnableAcceleration());

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_startup_pack_binds_only_when_every_lower_is_an_oss_object) {
    ImageConfig cfg;
    cfg.repo_blob_url = "oss://bucket/blobs";
    std::vector<StartupPackLayerRef> refs;

    // No lowers at all: nothing to bind.
    MT_EXPECT_TRUE(!CollectStartupPackLayers(cfg, &refs));

    LayerConfig oss_layer;
    oss_layer.digest = "sha256:" + std::string(64, 'a');
    oss_layer.size = kBlk;
    cfg.lowers.push_back(oss_layer);
    MT_EXPECT_TRUE(CollectStartupPackLayers(cfg, &refs));
    MT_EXPECT_EQ(static_cast<long long>(refs.size()), static_cast<long long>(1));
    if (refs.size() == 1) {
        MT_EXPECT_TRUE(refs[0].url == "oss://bucket/blobs/" + oss_layer.digest);
        MT_EXPECT_EQ(static_cast<long long>(refs[0].size), static_cast<long long>(kBlk));
    }

    // One local lower is enough to refuse the whole pack: it binds by exact
    // object-table match, so a partial binding is useless.
    LayerConfig local_layer;
    local_layer.file = "/tmp/some-local-layer";
    cfg.lowers.push_back(local_layer);
    MT_EXPECT_TRUE(!CollectStartupPackLayers(cfg, &refs));
    MT_EXPECT_EQ(static_cast<long long>(refs.size()), static_cast<long long>(0));

    // A lower with no size is unbindable too.
    cfg.lowers.clear();
    LayerConfig sizeless;
    sizeless.digest = "sha256:" + std::string(64, 'b');
    cfg.lowers.push_back(sizeless);
    MT_EXPECT_TRUE(!CollectStartupPackLayers(cfg, &refs));

    // And a non-OSS repo url, even with everything else present.
    cfg.repo_blob_url = "https://example.com/v2/repo/blobs";
    cfg.lowers.clear();
    cfg.lowers.push_back(oss_layer);
    MT_EXPECT_TRUE(!CollectStartupPackLayers(cfg, &refs));
}

MT_TEST(a_terminal_restack_failure_is_distinguishable) {
    const std::string wrapped = MakeRestackSnapshotTerminalFailure("disk went away");
    MT_EXPECT_TRUE(IsRestackSnapshotTerminalFailure(wrapped));
    MT_EXPECT_TRUE(wrapped.find("disk went away") != std::string::npos);
    // An ordinary failure must not be mistaken for a terminal one: the two
    // call for different recovery.
    MT_EXPECT_TRUE(!IsRestackSnapshotTerminalFailure("disk went away"));
    MT_EXPECT_TRUE(!IsRestackSnapshotTerminalFailure(""));
}

MT_TEST(an_upper_without_its_index_cannot_be_sealed) {
    UpperConfig upper;
    upper.has_mode = true;
    upper.mode = UpperMode::LogStructured;
    upper.data = "/tmp/upper.data";
    // A log-structured upper keeps its mappings in a separate index; sealing
    // one without that index would produce a layer that owns nothing.
    MT_EXPECT_TRUE(!EnsureSealableUpper(upper, "close_seal").ok());

    upper.index = "/tmp/upper.index";
    MT_EXPECT_TRUE(EnsureSealableUpper(upper, "close_seal").ok());

    // Sparse carries its index inside the data file, so the data path is all
    // it needs.
    UpperConfig sparse;
    sparse.has_mode = true;
    sparse.mode = UpperMode::Sparse;
    sparse.data = "/tmp/upper.data";
    MT_EXPECT_TRUE(EnsureSealableUpper(sparse, "close_seal").ok());

    // No data path at all is never sealable.
    UpperConfig empty;
    MT_EXPECT_TRUE(!EnsureSealableUpper(empty, "close_seal").ok());
}


// ============================================================
// alignment with the Rust open path
// ============================================================

MT_TEST(an_unprobeable_layer_dir_fails_the_open_instead_of_looking_absent) {
    // Rust guards the commit/sealed probe with `try_exists(..).await?`, so a
    // directory it cannot stat fails the open. Reporting "no local file"
    // instead would send the layer down the remote path and blame a missing
    // digest for what is really an unreadable directory.
    //
    // The condition is created with a symlink loop rather than by removing
    // permissions: these tests also run as root, which bypasses the directory
    // search bit, and ELOOP is refused by the kernel for every user.
    const std::string dir = MakeTempDir();
    const std::string loop_a = core::fs::Join(dir, "loop-a");
    const std::string loop_b = core::fs::Join(dir, "loop-b");
    MT_EXPECT_TRUE(::symlink(loop_b.c_str(), loop_a.c_str()) == 0);
    MT_EXPECT_TRUE(::symlink(loop_a.c_str(), loop_b.c_str()) == 0);

    ImageConfig cfg;
    cfg.repo_blob_url = "https://example.invalid/v2/repo/blobs";
    LayerConfig layer;
    layer.dir = loop_a;
    // A digest is present on purpose: without the error propagating, this
    // layer would quietly be fetched from the registry instead.
    layer.digest = "sha256:" + std::string(64, 'c');
    layer.size = kBlk;
    cfg.lowers.push_back(layer);

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(!image.ok());
    if (!image.ok()) {
        // The reason must name the probe, not a missing digest and not a
        // remote fetch failure.
        MT_EXPECT_TRUE(image.error().find("stat") != std::string::npos);
        MT_EXPECT_TRUE(image.error().find("no local file and no digest") == std::string::npos);
    }

    RemoveTreeBestEffort(dir);
}

MT_TEST(try_exists_separates_absence_from_inaccessibility) {
    const std::string dir = MakeTempDir();

    // Present.
    const std::string present = core::fs::Join(dir, "here");
    MT_EXPECT_TRUE(core::fs::Write(present, "x").ok());
    core::Expected<bool, std::string> r = core::fs::TryExists(present);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value());

    // Absent is an answer, not a failure.
    r = core::fs::TryExists(core::fs::Join(dir, "nope"));
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(!r.value());

    // A non-directory component behaves as absence, as ENOTDIR does in Rust.
    r = core::fs::TryExists(core::fs::Join(present, "under-a-file"));
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(!r.value());

    // The empty path reports absence rather than erroring.
    r = core::fs::TryExists("");
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(!r.value());

    // Inaccessible is neither, and this is the case `Exists` cannot express.
    // A symlink loop rather than a permission bit, so the assertion holds for
    // root too.
    const std::string loop_a = core::fs::Join(dir, "loop-a");
    const std::string loop_b = core::fs::Join(dir, "loop-b");
    MT_EXPECT_TRUE(::symlink(loop_b.c_str(), loop_a.c_str()) == 0);
    MT_EXPECT_TRUE(::symlink(loop_a.c_str(), loop_b.c_str()) == 0);

    r = core::fs::TryExists(loop_a);
    MT_EXPECT_TRUE(!r.ok());
    // Exactly where `Exists` would have said "no", which is the whole reason
    // the two functions are separate.
    MT_EXPECT_TRUE(!core::fs::Exists(loop_a));

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_restack_to_a_bare_file_name_is_accepted) {
    // Rust refuses only where `Path::parent()` is absent -- the root and the
    // empty path. For a bare file name it gets `Some("")` and
    // `create_dir_all("")` succeeds, so a relative output path is allowed.
    // `core::fs::Parent` reports absence for both, which is why the two cases
    // are separated by hand.
    const std::string dir = MakeTempDir();
    char cwd[4096];
    const char* got_cwd = ::getcwd(cwd, sizeof(cwd));
    MT_EXPECT_TRUE(got_cwd != 0);
    if (got_cwd == 0) {
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(::chdir(dir.c_str()) == 0);

    ImageConfig cfg;
    AddUpper(&cfg, dir);
    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        std::fprintf(stderr, "  open: %s\n", image.error().c_str());
        if (::chdir(cwd) != 0) { /* best effort */ }
        RemoveTreeBestEffort(dir);
        return;
    }

    const std::vector<uint8_t> payload = Pattern(static_cast<size_t>(kBlk), 0x22);
    MT_EXPECT_TRUE(image.value()->WriteAt(0, &payload[0], payload.size()).ok());

    const core::Expected<core::Optional<LayerDescriptor>, std::string> restacked =
        image.value()->CreateSnapshotAndRestack("bare-snap.lsmt");
    MT_EXPECT_TRUE(restacked.ok());
    if (!restacked.ok()) std::fprintf(stderr, "  restack: %s\n", restacked.error().c_str());
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(dir, "bare-snap.lsmt")));

    // The root still has no parent and is still refused.
    const core::Expected<core::Optional<LayerDescriptor>, std::string> rooted =
        image.value()->CreateSnapshotAndRestack("/");
    MT_EXPECT_TRUE(!rooted.ok());
    if (!rooted.ok()) {
        MT_EXPECT_TRUE(rooted.error().find("no parent directory") != std::string::npos);
    }

    if (::chdir(cwd) != 0) { /* best effort */ }
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_seal_to_a_bare_file_name_needs_no_directory_created) {
    const std::string dir = MakeTempDir();
    char cwd[4096];
    const char* got_cwd = ::getcwd(cwd, sizeof(cwd));
    MT_EXPECT_TRUE(got_cwd != 0);
    if (got_cwd == 0) {
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(::chdir(dir.c_str()) == 0);

    ImageConfig cfg;
    AddUpper(&cfg, dir);
    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(cfg, service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        if (::chdir(cwd) != 0) { /* best effort */ }
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<core::Optional<LayerDescriptor>, std::string> sealed =
        image.value()->CloseSeal("bare-sealed.lsmt");
    MT_EXPECT_TRUE(sealed.ok());
    if (!sealed.ok()) std::fprintf(stderr, "  seal: %s\n", sealed.error().c_str());
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(dir, "bare-sealed.lsmt")));

    if (::chdir(cwd) != 0) { /* best effort */ }
    RemoveTreeBestEffort(dir);
}

MT_TEST(size_reports_the_images_virtual_size_and_updates_the_counters) {
    // Narrower than it may look, and deliberately named for what it checks.
    // Rust's `size()` *is* `refresh_size_metadata`, so it returns the value
    // that call measured rather than a re-read of the counter; the port
    // matches that. The difference is only observable when another thread
    // stores between the two, so it is **not** covered here -- mutation
    // testing confirmed a re-reading variant survives this test. What is
    // covered is that the value is right and the counters agree with it.
    const std::string dir = MakeTempDir();
    std::vector<uint64_t> b0;
    b0.push_back(0);
    std::vector<std::string> layers;
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", b0, 0x10));

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<uint64_t, std::string> size = image.value()->Size();
    MT_EXPECT_TRUE(size.ok());
    if (size.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(size.value()),
                     static_cast<long long>(kVirtualSize));
        // And the cached counters agree with it.
        MT_EXPECT_EQ(static_cast<long long>(image.value()->SizeBytes()),
                     static_cast<long long>(size.value()));
        MT_EXPECT_EQ(static_cast<long long>(image.value()->NumLbas()),
                     static_cast<long long>(size.value() / kImageDefaultBlockSize));
    }

    RemoveTreeBestEffort(dir);
}

MT_TEST(the_data_path_takes_a_shared_guard_not_an_exclusive_one) {
    // Rust takes a *shared* guard for every data-path operation and an
    // exclusive one only where the layer stack is replaced. A plain mutex, or
    // an exclusive guard here, would serialise reads of a block device's
    // backing image -- a change in concurrency, not in implementation.
    //
    // Honest about what this measures: `core::RwLock`'s sharing is proven
    // deterministically in `test_core_rwlock`. This test can only observe
    // which guard `ReadAt` chose, and the sole external difference is
    // elapsed time, so it compares against a serialised baseline taken in
    // the same run. Mutation-checked against swapping in `WriteGuard`.
    const std::string dir = MakeTempDir();
    std::vector<uint64_t> blocks;
    for (uint64_t i = 0; i < 16; ++i) blocks.push_back(i);
    std::vector<std::string> layers;
    layers.push_back(WriteSealedLayer(dir, "l0.lsmt", "layer-0-uuid", blocks, 0x10));

    ImageService service = MakeService(dir);
    const core::Expected<std::shared_ptr<ImageFile>, std::string> image =
        ImageFile::Open(LocalLowerImageConfig(layers), service, core::Optional<std::string>());
    MT_EXPECT_TRUE(image.ok());
    if (!image.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    // Large enough that one read is real work, so the two runs differ by more
    // than scheduling noise.
    const size_t kBytes = static_cast<size_t>(16 * kBlk);
    const size_t kIterations = 400;

    size_t failures = 0;
    // Warm the page cache first, so the first run does not pay for it alone.
    TimeReads(image.value().get(), false, 20, kBytes, &failures);
    MT_EXPECT_EQ(static_cast<long long>(failures), static_cast<long long>(0));

    const uint64_t serialised_ms =
        TimeReads(image.value().get(), true, kIterations, kBytes, &failures);
    MT_EXPECT_EQ(static_cast<long long>(failures), static_cast<long long>(0));
    const uint64_t concurrent_ms =
        TimeReads(image.value().get(), false, kIterations, kBytes, &failures);
    MT_EXPECT_EQ(static_cast<long long>(failures), static_cast<long long>(0));

    // A loose bound on purpose: the claim is "these ran in parallel", not a
    // speedup figure. An exclusive guard makes the two runs equal, because
    // the test's own mutex then adds nothing.
    const bool parallel = concurrent_ms * 10 < serialised_ms * 8;
    if (!parallel) {
        std::fprintf(stderr, "  serialised=%llums concurrent=%llums\n",
                     static_cast<unsigned long long>(serialised_ms),
                     static_cast<unsigned long long>(concurrent_ms));
    }
    MT_EXPECT_TRUE(parallel);

    RemoveTreeBestEffort(dir);
}

MT_MAIN
