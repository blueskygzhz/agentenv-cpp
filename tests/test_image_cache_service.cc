// SPDX-License-Identifier: MIT
// Rust: src/image/cache/service.rs and src/image/cache/source_config.rs tests.
//
// `published_posix_image_does_not_own_or_charge_local_commits` drives
// `ImageResolver::resolve`/`publish_image` and is ported with the resolver.
#include "agentenv/image/cache_service.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/image/commit_index.h"
#include "agentenv/p2p/mock.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "microtest.h"

using namespace agentenv;                // NOLINT
using namespace agentenv::image;         // NOLINT
using namespace agentenv::image::cache;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        core::Expected<std::string, std::string> d =
            core::fs::CreateTempDir("agentenv-image-cache-service-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-image-cache-service-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

std::shared_ptr<ImageCacheService> TestService(const TempRoot& temp) {
    cfg::ResolvedImageCacheConfig config;
    config.root_dir = temp.path + "/image-cache";
    config.commit_store = config.root_dir + "/commits";
    config.remote_blocks_dir = config.root_dir + "/remote-blocks";
    config.remote_blocks_size_gb = 10;
    return ImageCacheService::FromResolvedConfig(config);
}

core::Json Lower(const std::string& file, const std::string& digest, uint64_t size) {
    core::Json lower = core::Json(core::JsonObject());
    lower["file"] = file;
    lower["digest"] = digest;
    lower["size"] = static_cast<int64_t>(size);
    return lower;
}

core::Json Lowers(const core::Json& one) {
    core::Json lowers = core::Json(core::JsonArray());
    lowers.push_back(one);
    return lowers;
}

core::Json ImageConfigJson(const std::string& repo_blob_url, const core::Json& lowers) {
    core::Json config = core::Json(core::JsonObject());
    config["repoBlobUrl"] = repo_blob_url;
    config["lowers"] = lowers;
    config["upper"] = core::Json(core::JsonObject());
    config["resultFile"] = "";
    return config;
}

void WriteImageConfig(const std::string& path, const std::string& repo_blob_url,
                      const core::Json& lowers) {
    MT_EXPECT_TRUE(core::fs::CreateDirAll(*core::fs::Parent(path)).ok());
    MT_EXPECT_TRUE(
        core::fs::Write(path, ImageConfigJson(repo_blob_url, lowers).ToStringPretty()).ok());
}

void WriteConfig(const std::string& path) {
    WriteImageConfig(path, "https://registry.example/v2/repo/blobs",
                     Lowers(Lower("../commits/sha256-hard/overlaybd.commit", "sha256:hard", 11)));
}

std::string WriteCommitFile(const ImageCacheService& service, const std::string& digest,
                            const std::string& payload) {
    const std::string path = CommitFile(service.commit_store(), digest);
    MT_EXPECT_TRUE(core::fs::CreateDirAll(*core::fs::Parent(path)).ok());
    MT_EXPECT_TRUE(core::fs::Write(path, payload).ok());
    return path;
}

void AssertImportedCommitIsCollectable(const std::shared_ptr<ImageCacheService>& service,
                                       const std::string& cached, const std::string& digest,
                                       const std::string& payload) {
    MT_EXPECT_EQ(cached, CommitFile(service->commit_store(), digest));
    core::Expected<std::string, std::string> read = core::fs::ReadToString(cached);
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_EQ(read.value(), payload);

    core::Expected<ImageCacheGcReport, std::string> report =
        service->RunGc(ImageCacheLiveRuntimeRefs(), true);
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(report.value().freed_bytes, static_cast<uint64_t>(payload.size()));
    MT_EXPECT_TRUE(!core::fs::Exists(cached));
}

void WriteTestIndex(const ImageCacheService& service, const std::string& source_digest,
                    const std::string& commit_digest) {
    const CommitIndex index = CommitIndex::OciLayer(source_digest, commit_digest, 4,
                                                    "test-converter", 1, false,
                                                    core::Optional<std::string>());
    MT_EXPECT_TRUE(index
                       .Write(IndexPath(service.index_dir(), source_digest, "test-converter", 1,
                                        false, core::Optional<std::string>()))
                       .ok());
}

HardCommitId Hard(const std::string& digest) {
    core::Expected<HardCommitId, std::string> h = HardCommitId::New(digest);
    MT_EXPECT_TRUE(h.ok());
    return h.value();
}

std::set<HardCommitId> HardRefs(const std::vector<std::string>& digests) {
    std::set<HardCommitId> out;
    for (std::size_t i = 0; i < digests.size(); ++i) out.insert(Hard(digests[i]));
    return out;
}

ImageCacheHoldOwner Owner(const std::string& ns, const std::string& key) {
    core::Expected<ImageCacheHoldOwner, std::string> o = ImageCacheHoldOwner::New(ns, key);
    MT_EXPECT_TRUE(o.ok());
    return o.value();
}

/// Rust `write_sealed_layer` — a real sealed overlaybd layer stamped with
/// `uuid`, 4 KiB of 0x5a in an 8 KiB virtual device.
void WriteSealedLayer(const std::string& path, const core::Uuid& uuid) {
    namespace ob = storage::overlaybd;
    const std::string index_path = path.substr(0, path.rfind('.')) + ".index";
    core::Expected<std::shared_ptr<ob::LocalFile>, std::string> data = ob::LocalFile::New(path);
    core::Expected<std::shared_ptr<ob::LocalFile>, std::string> index =
        ob::LocalFile::New(index_path);
    MT_EXPECT_TRUE(data.ok() && index.ok());
    core::Expected<std::shared_ptr<ob::LsmtFile>, std::string> file =
        ob::LsmtFile::CreateWithMetadata(data.value(), index.value(), 8192,
                                         ob::kRwLogStructured, uuid.ToString(), std::string(),
                                         NULL, 0);
    MT_EXPECT_TRUE(file.ok());
    const std::vector<uint8_t> block(4096, 0x5a);
    MT_EXPECT_TRUE(file.value()->WriteAt(0, &block[0], block.size()).ok());
    MT_EXPECT_TRUE(file.value()->CloseSeal().ok());
}

/// Rust `test_conversion_key` — matches `WriteTestIndex`'s context.
LayerConversionKey TestConversionKey(const std::string& source_digest,
                                     const core::Uuid& expected_uuid) {
    LayerConversionKey key;
    key.source_layer_digest = source_digest;
    key.converter_id = "test-converter";
    key.virtual_size_gib = 1;
    key.mkfs = false;
    key.expected_layer_uuid = expected_uuid;
    return key;
}

LocalLayer Layer(const std::string& path, const std::string& digest, uint64_t size) {
    LocalLayer layer;
    layer.path = path;
    layer.digest = digest;
    layer.size = size;
    return layer;
}

HardCommitObjectRecord CommitRecord(const std::shared_ptr<ImageCacheService>& service,
                                    const std::string& digest) {
    core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        service->MetadataStore();
    MT_EXPECT_TRUE(store.ok());
    core::Expected<core::Optional<HardCommitObjectRecord>, std::string> record =
        store.value()->GetHardCommitObject(Hard(digest));
    MT_EXPECT_TRUE(record.ok());
    MT_EXPECT_TRUE(record.value().has_value());
    return *record.value();
}

std::map<std::string, ImageCacheGcBlockedReason> BlockedReasons(
    const ImageCacheGcReport& report) {
    std::map<std::string, ImageCacheGcBlockedReason> out;
    for (std::size_t i = 0; i < report.blocked.size(); ++i) {
        out[report.blocked[i].digest.AsStr()] = report.blocked[i].reason;
    }
    return out;
}

bool ReasonIs(const std::map<std::string, ImageCacheGcBlockedReason>& reasons,
              const std::string& digest, ImageCacheGcBlockedReason::Kind kind) {
    std::map<std::string, ImageCacheGcBlockedReason>::const_iterator it = reasons.find(digest);
    return it != reasons.end() && it->second.kind == kind;
}

std::vector<ImageCacheHoldOwner> OwnersOf(
    const std::map<std::string, ImageCacheGcBlockedReason>& reasons, const std::string& digest) {
    std::map<std::string, ImageCacheGcBlockedReason>::const_iterator it = reasons.find(digest);
    return it == reasons.end() ? std::vector<ImageCacheHoldOwner>() : it->second.owners;
}

}  // namespace

// ---- source_config.rs -------------------------------------------------------

MT_TEST(source_image_config_filename_scopes_and_bounds_remote_repository) {
    const std::string first_scope = "registry.example.com/team/" + std::string(160, 'x');
    const std::string second_scope = "registry.example.com/team/" + std::string(160, 'y');
    const std::string first = SourceImageConfigFilename(
        ImageCacheSourceImageKey::New("sha256:abc123", core::Optional<std::string>(first_scope))
            .value());
    const std::string first_again = SourceImageConfigFilename(
        ImageCacheSourceImageKey::New("sha256:abc123", core::Optional<std::string>(first_scope))
            .value());
    const std::string second = SourceImageConfigFilename(
        ImageCacheSourceImageKey::New("sha256:abc123", core::Optional<std::string>(second_scope))
            .value());

    MT_EXPECT_TRUE(first.find('/') == std::string::npos);
    MT_EXPECT_TRUE(first.size() < 255);
    MT_EXPECT_TRUE(first.compare(0, 40, "sha256-abc123-registry.example.com-team-") == 0);
    const std::string suffix = "-image.json";
    MT_EXPECT_TRUE(first.size() > suffix.size() &&
                   first.compare(first.size() - suffix.size(), suffix.size(), suffix) == 0);
    MT_EXPECT_EQ(first, first_again);
    MT_EXPECT_TRUE(first != second);
}

MT_TEST(source_image_config_cache_usability_tracks_local_file_requirements) {
    TempRoot temp;
    const std::string missing_local = temp.path + "/missing-local.json";
    const std::string remote_only = temp.path + "/remote-only.json";
    MT_EXPECT_TRUE(core::fs::Write(missing_local,
                                   ImageConfigJson("", Lowers(Lower(temp.path + "/missing.commit",
                                                                    "sha256:missing", 7)))
                                       .ToString())
                       .ok());
    core::Json remote_lower = core::Json(core::JsonObject());
    remote_lower["digest"] = "sha256:abc";
    remote_lower["size"] = static_cast<int64_t>(123);
    MT_EXPECT_TRUE(
        core::fs::Write(remote_only, ImageConfigJson("https://registry.example/v2/repo/blobs",
                                                     Lowers(remote_lower))
                                         .ToString())
            .ok());

    MT_EXPECT_TRUE(!SourceImageConfigIsUsable(missing_local));
    MT_EXPECT_TRUE(SourceImageConfigIsUsable(remote_only));
}

MT_TEST(source_image_lock_registry_drops_released_entries) {
    std::shared_ptr<SourceImageCache> cache(new SourceImageCache());
    {
        std::shared_ptr<SourceImageLock> a = cache->AcquireLock("/tmp/a-image.json");
        std::shared_ptr<SourceImageLock> b = cache->AcquireLock("/tmp/b-image.json");
        MT_EXPECT_EQ(cache->RegisteredLocksForTesting(), static_cast<std::size_t>(2));
    }
    MT_EXPECT_EQ(cache->RegisteredLocksForTesting(), static_cast<std::size_t>(0));
}

// ---- service.rs -------------------------------------------------------------

MT_TEST(lookup_converted_layer_misses_for_missing_or_invalid_commit) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    MT_EXPECT_TRUE(service->EnsureLayout().ok());
    WriteTestIndex(*service, "sha256:src", "sha256:commit-missing");
    WriteTestIndex(*service, "sha256:bad-src", "sha256:commit-bad");
    // 4 bytes matches the indexed size, so the lookup passes the size gate and
    // is rejected purely because the bytes are not a sealed overlaybd layer.
    WriteCommitFile(*service, "sha256:commit-bad", "junk");

    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> hold =
        service->BeginHardCommitOperation("test");
    MT_EXPECT_TRUE(hold.ok());
    core::Expected<core::Optional<LocalLayer>, std::string> found =
        hold.value()->LookupConvertedLayer(TestConversionKey("sha256:src", core::Uuid()));
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(!found.value().has_value());
    found = hold.value()->LookupConvertedLayer(TestConversionKey("sha256:bad-src", core::Uuid()));
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(!found.value().has_value());
}

MT_TEST(publish_converted_layer_records_p2p_ownership) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    MT_EXPECT_TRUE(service->EnsureLayout().ok());
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    service->InitializeP2pTransport(transport);

    const std::string digest = "sha256:converted";
    const std::string file = WriteCommitFile(*service, digest, "converted");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject(digest, core::Optional<std::string>(file),
                                                core::Optional<uint64_t>(9),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    const LayerConversionKey key = TestConversionKey("sha256:source", core::Uuid::GenV4());

    core::Expected<bool, std::string> published =
        service->PublishConvertedLayer(key, Layer(file, digest, 9));
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_TRUE(published.value());
    const p2p::P2pArtifactKey p2p_key = key.P2pKey();
    p2p::P2pArtifactDescriptor descriptor;
    MT_EXPECT_TRUE(transport->Descriptor(p2p_key, &descriptor));
    core::Expected<core::Json, core::AnyError> raw = core::Json::Parse(descriptor.metadata_json);
    MT_EXPECT_TRUE(raw.ok());
    core::Expected<ConvertedLayerP2pMetadata, std::string> metadata =
        ConvertedLayerP2pMetadata::Parse(raw.value());
    MT_EXPECT_TRUE(metadata.ok());
    MT_EXPECT_EQ(metadata.value().commit_digest, digest);
    MT_EXPECT_TRUE(CommitRecord(service, digest).p2p_keys.count(p2p_key) == 1);
}

MT_TEST(disabled_p2p_publish_does_not_record_ownership) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    MT_EXPECT_TRUE(service->EnsureLayout().ok());
    service->InitializeP2pTransport(
        std::shared_ptr<p2p::P2pTransport>(new p2p::DisabledP2pTransport()));

    const std::string digest = "sha256:disabled-p2p";
    const std::string file = WriteCommitFile(*service, digest, "converted");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject(digest, core::Optional<std::string>(file),
                                                core::Optional<uint64_t>(9),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    const LayerConversionKey key =
        TestConversionKey("sha256:disabled-source", core::Uuid::GenV4());

    core::Expected<bool, std::string> published =
        service->PublishConvertedLayer(key, Layer(file, digest, 9));
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_TRUE(!published.value());
    MT_EXPECT_TRUE(CommitRecord(service, digest).p2p_keys.empty());
}

MT_TEST(failed_converted_layer_publish_keeps_gc_ownership_record) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    MT_EXPECT_TRUE(service->EnsureLayout().ok());
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    transport->SetFailPublish(true);
    service->InitializeP2pTransport(transport);

    const std::string digest = "sha256:publish-failure";
    const std::string file = WriteCommitFile(*service, digest, "payload");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject(digest, core::Optional<std::string>(file),
                                                core::Optional<uint64_t>(7),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    const LayerConversionKey key = TestConversionKey("sha256:source-failure", core::Uuid::GenV4());

    MT_EXPECT_TRUE(!service->PublishConvertedLayer(key, Layer(file, digest, 7)).ok());
    MT_EXPECT_TRUE(CommitRecord(service, digest).p2p_keys.count(key.P2pKey()) == 1);
}

MT_TEST(lookup_converted_layer_rehydrates_from_p2p) {
    TempRoot provider_temp;
    TempRoot consumer_temp;
    std::shared_ptr<ImageCacheService> provider = TestService(provider_temp);
    std::shared_ptr<ImageCacheService> consumer = TestService(consumer_temp);
    MT_EXPECT_TRUE(provider->EnsureLayout().ok());
    MT_EXPECT_TRUE(consumer->EnsureLayout().ok());
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    provider->InitializeP2pTransport(transport);
    consumer->InitializeP2pTransport(transport);

    const LayerConversionKey key = TestConversionKey("sha256:source-p2p", core::Uuid::GenV4());
    const std::string provider_path = provider_temp.path + "/provider.commit";
    WriteSealedLayer(provider_path, key.expected_layer_uuid);
    core::Expected<core::FileDigest, std::string> described = core::DescribeFile(provider_path);
    MT_EXPECT_TRUE(described.ok());
    MT_EXPECT_TRUE(provider
                       ->RecordHardCommitObject(described.value().sha256,
                                                core::Optional<std::string>(provider_path),
                                                core::Optional<uint64_t>(described.value().size),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    MT_EXPECT_TRUE(provider
                       ->PublishConvertedLayer(key, Layer(provider_path, described.value().sha256,
                                                          described.value().size))
                       .ok());

    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> hold =
        consumer->BeginHardCommitOperation("p2p-rehydrate");
    MT_EXPECT_TRUE(hold.ok());
    core::Expected<core::Optional<LocalLayer>, std::string> layer =
        hold.value()->LookupConvertedLayer(key);
    MT_EXPECT_TRUE(layer.ok());
    MT_EXPECT_TRUE(layer.value().has_value());
    MT_EXPECT_EQ(layer.value()->digest, described.value().sha256);
    MT_EXPECT_EQ(layer.value()->size, described.value().size);
    MT_EXPECT_TRUE(core::fs::Exists(layer.value()->path));
    MT_EXPECT_TRUE(CommitRecord(consumer, described.value().sha256).p2p_keys.count(key.P2pKey()) ==
                   1);
}

MT_TEST(failed_p2p_fetch_is_attempted_once_per_operation) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    MT_EXPECT_TRUE(service->EnsureLayout().ok());
    std::shared_ptr<p2p::MockTransport> transport(new p2p::MockTransport());
    service->InitializeP2pTransport(transport);

    const LayerConversionKey key = TestConversionKey("sha256:stale-p2p", core::Uuid::GenV4());
    const p2p::P2pArtifactKey p2p_key = key.P2pKey();
    const LocalLayer advertised_layer =
        Layer(temp.path + "/missing.commit", "sha256:missing-p2p-blob", 123);
    p2p::P2pArtifactDescriptor descriptor;
    descriptor.key = p2p_key;
    descriptor.providers.push_back(p2p::P2pArtifactProvider::MakeLocal());
    descriptor.has_backend_locator = true;
    descriptor.backend_locator = "missing-mock-blob";
    descriptor.metadata_json =
        ConvertedLayerP2pMetadata::FromLocalLayer(advertised_layer).ToJson().ToString();
    transport->InsertDescriptor(descriptor);

    {
        core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> first_hold =
            service->BeginHardCommitOperation("first-p2p-fetch");
        MT_EXPECT_TRUE(first_hold.ok());
        for (int i = 0; i < 2; ++i) {
            core::Expected<core::Optional<LocalLayer>, std::string> found =
                first_hold.value()->LookupConvertedLayer(key);
            MT_EXPECT_TRUE(found.ok());
            MT_EXPECT_TRUE(!found.value().has_value());
        }
        MT_EXPECT_EQ(transport->LookupCount(), 1);
        MT_EXPECT_EQ(transport->FetchCount(), 1);
    }

    core::Expected<std::shared_ptr<OperationHoldConversion>, std::string> second_hold =
        service->BeginHardCommitOperation("second-p2p-fetch");
    MT_EXPECT_TRUE(second_hold.ok());
    core::Expected<core::Optional<LocalLayer>, std::string> found =
        second_hold.value()->LookupConvertedLayer(key);
    MT_EXPECT_TRUE(found.ok());
    MT_EXPECT_TRUE(!found.value().has_value());
    MT_EXPECT_EQ(transport->LookupCount(), 2);
    MT_EXPECT_EQ(transport->FetchCount(), 2);
}

MT_TEST(import_hard_commit_publishes_facts_and_releases_operation_hold) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string payload = "trusted descriptor commit";
    const std::string source = temp.path + "/trusted.commit";
    MT_EXPECT_TRUE(core::fs::Write(source, payload).ok());
    const std::string digest = "sha256:trusted-descriptor";

    core::Expected<std::string, std::string> cached =
        service->ImportHardCommitTrustedDescriptor(source, digest, payload.size());
    MT_EXPECT_TRUE(cached.ok());

    AssertImportedCommitIsCollectable(service, cached.value(), digest, payload);
}

MT_TEST(publish_source_image_config_writes_files_and_records_refs) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string manifest_digest = "sha256:manifest";
    const core::Optional<std::string> repository_scope(std::string("repo/app"));
    const std::string commit_file = WriteCommitFile(*service, "sha256:layer", "payload");
    const core::Json config = ImageConfigJson("", Lowers(Lower(commit_file, "sha256:layer", 7)));
    ImageResolutionMetadata metadata;
    metadata.base_context.workdir = core::Optional<std::string>(std::string("/work"));
    core::Json raw = core::Json(core::JsonObject());
    raw["architecture"] = "x86_64";
    metadata.raw_config = core::Optional<core::Json>(raw);

    core::Expected<std::shared_ptr<ImageConversion>, std::string> conversion =
        service->BeginImageConversion(manifest_digest, repository_scope);
    MT_EXPECT_TRUE(conversion.ok());
    core::Expected<std::string, std::string> published_path = service->PublishImageConfig(
        manifest_digest, repository_scope, config, metadata, conversion.value());
    // Hand over the only reference, as Rust moves `conversion` in.
    conversion.value().reset();
    MT_EXPECT_TRUE(published_path.ok());

    MT_EXPECT_TRUE(core::fs::Exists(published_path.value()));
    core::Expected<CachedImageConfig, std::string> cached =
        service->LookupImageConfig(manifest_digest, repository_scope);
    MT_EXPECT_TRUE(cached.ok());
    MT_EXPECT_TRUE(cached.value().kind == CachedImageConfig::Kind::Found);
    MT_EXPECT_TRUE(cached.value().metadata.has_value());
    MT_EXPECT_TRUE(*cached.value().metadata == metadata);

    core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> store =
        service->MetadataStore();
    MT_EXPECT_TRUE(store.ok());
    core::Expected<std::map<HardCommitId, std::vector<ImageCacheConfigId> >, std::string>
        referrers = store.value()->HardCommitConfigReferrerMap();
    MT_EXPECT_TRUE(referrers.ok());
    std::vector<ImageCacheConfigId> expected;
    expected.push_back(ImageCacheConfigId::FromConfigPath(published_path.value()).value());
    MT_EXPECT_TRUE(referrers.value()[Hard("sha256:layer")] == expected);
}

MT_TEST(overlaybd_layer_location_binds_file_for_cache_local_and_dir_for_remote) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string payload = "layerbytes";
    const std::string commit_file = WriteCommitFile(*service, "sha256:layer", payload);
    const uint64_t size = payload.size();

    // Cache-local layer with a usable cached commit binds to `file=`.
    OverlaybdLayerLocation location = service->LayerLocation("sha256:layer", size, false);
    MT_EXPECT_TRUE(location.kind == OverlaybdLayerLocation::Kind::LocalFile);
    MT_EXPECT_EQ(location.path, commit_file);

    // A remote-recoverable layer must use `dir=` even when the commit is
    // cached, so the local commit stays GC-reclaimable.
    location = service->LayerLocation("sha256:layer", size, true);
    MT_EXPECT_TRUE(location.kind == OverlaybdLayerLocation::Kind::CacheDir);
    MT_EXPECT_EQ(location.path, CommitDir(service->commit_store(), "sha256:layer"));

    // Local-only layer without a usable cached commit falls back to `dir=`.
    location = service->LayerLocation("sha256:absent", size, false);
    MT_EXPECT_TRUE(location.kind == OverlaybdLayerLocation::Kind::CacheDir);
}

MT_TEST(run_gc_collects_free_candidate_and_skips_roots_holds_and_live_refs) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    std::shared_ptr<p2p::MockTransport> p2p(new p2p::MockTransport());
    service->InitializeP2pTransport(p2p);
    const std::string free_file = WriteCommitFile(*service, "sha256:free", "free");
    const std::string rooted_file = WriteCommitFile(*service, "sha256:rooted", "rooted");
    const std::string held_file = WriteCommitFile(*service, "sha256:held", "held");
    const std::string live_file = WriteCommitFile(*service, "sha256:live", "live");
    std::vector<p2p::P2pArtifactKey> free_keys;
    free_keys.push_back("overlaybd-layer/v1/explicit-free");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:free", core::Optional<std::string>(free_file),
                                                core::Optional<uint64_t>(4), free_keys)
                       .ok());
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:rooted",
                                                core::Optional<std::string>(rooted_file),
                                                core::Optional<uint64_t>(6),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:held", core::Optional<std::string>(held_file),
                                                core::Optional<uint64_t>(4),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:live", core::Optional<std::string>(live_file),
                                                core::Optional<uint64_t>(4),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());
    const std::string source_config = service->config_dir() + "/base-image.json";
    WriteImageConfig(source_config, "", Lowers(Lower(rooted_file, "sha256:rooted", 6)));
    MT_EXPECT_TRUE(service->RecordSourceConfigRefsFromConfigPath(source_config).ok());
    const ImageCacheHoldOwner paused_owner = Owner("paused", "sandbox-paused");
    MT_EXPECT_TRUE(
        service->CreateOrReplaceHold(paused_owner, HardRefs(std::vector<std::string>(
                                                       1, "sha256:held")))
            .ok());

    const ImageCacheHoldOwner live_owner = Owner("runtime", "sandbox-live");
    ImageCacheLiveRuntimeRefs live_refs;
    live_refs[Hard("sha256:live")].push_back(live_owner);
    core::Expected<ImageCacheGcReport, std::string> report = service->RunGc(live_refs, true);
    MT_EXPECT_TRUE(report.ok());

    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(1));
    MT_EXPECT_EQ(report.value().freed_bytes, static_cast<uint64_t>(4));
    MT_EXPECT_TRUE(!core::fs::Exists(free_file));
    MT_EXPECT_TRUE(p2p->UnpublishedKeys() == free_keys);
    MT_EXPECT_TRUE(core::fs::Exists(held_file));
    MT_EXPECT_TRUE(core::fs::Exists(live_file));
    MT_EXPECT_TRUE(core::fs::Exists(rooted_file));

    const std::map<std::string, ImageCacheGcBlockedReason> reasons =
        BlockedReasons(report.value());
    MT_EXPECT_TRUE(ReasonIs(reasons, "sha256:held", ImageCacheGcBlockedReason::Held));
    MT_EXPECT_TRUE(OwnersOf(reasons, "sha256:held") ==
                   std::vector<ImageCacheHoldOwner>(1, paused_owner));
    MT_EXPECT_TRUE(ReasonIs(reasons, "sha256:live", ImageCacheGcBlockedReason::LiveRuntime));
    MT_EXPECT_TRUE(OwnersOf(reasons, "sha256:live") ==
                   std::vector<ImageCacheHoldOwner>(1, live_owner));
    MT_EXPECT_TRUE(ReasonIs(reasons, "sha256:rooted", ImageCacheGcBlockedReason::RootedByConfig));
    MT_EXPECT_TRUE(reasons.find("sha256:rooted")->second.configs ==
                   std::vector<std::string>(1, "base-image.json"));
}

MT_TEST(run_gc_refuses_p2p_object_without_initialized_transport) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string file = WriteCommitFile(*service, "sha256:p2p", "p2p");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:p2p", core::Optional<std::string>(file),
                                                core::Optional<uint64_t>(3),
                                                std::vector<p2p::P2pArtifactKey>(
                                                    1, "overlaybd-layer/v1/p2p"))
                       .ok());

    core::Expected<ImageCacheGcReport, std::string> report =
        service->RunGc(ImageCacheLiveRuntimeRefs(), true);
    MT_EXPECT_TRUE(report.ok());

    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(core::fs::Exists(file));
    bool found = false;
    for (std::size_t i = 0; i < report.value().blocked.size(); ++i) {
        const ImageCacheGcBlocked& blocked = report.value().blocked[i];
        if (blocked.digest.AsStr() == "sha256:p2p" &&
            blocked.reason.kind == ImageCacheGcBlockedReason::DeleteFailed &&
            blocked.reason.detail.find("P2P transport is not initialized") != std::string::npos) {
            found = true;
        }
    }
    MT_EXPECT_TRUE(found);
}

MT_TEST(runtime_config_hold_tracks_only_commit_store_commits) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string leased = WriteCommitFile(*service, "sha256:leased", "leased");
    const std::string outside_file = temp.path + "/elsewhere/foreign.commit";
    const std::string outside_file_only = temp.path + "/elsewhere/runtime.commit";
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:leased", core::Optional<std::string>(leased),
                                                core::Optional<uint64_t>(6),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());

    const std::string runtime_config = temp.path + "/runtime/image.json";
    core::Json lowers = core::Json(core::JsonArray());
    lowers.push_back(Lower(leased, "sha256:leased", 6));
    lowers.push_back(Lower(outside_file, "sha256:outside", 7));
    core::Json file_only = core::Json(core::JsonObject());
    file_only["file"] = outside_file_only;
    lowers.push_back(file_only);
    core::Json remote = core::Json(core::JsonObject());
    remote["dir"] = "../commits/sha256-remote";
    remote["digest"] = "sha256:remote";
    remote["size"] = static_cast<int64_t>(9);
    lowers.push_back(remote);
    WriteImageConfig(runtime_config, "https://registry.example/v2/repo/blobs", lowers);

    const ImageCacheHoldOwner owner = Owner("runtime", "sandbox-running");
    MT_EXPECT_TRUE(
        service->AcquireHoldForConfigPaths(owner, std::vector<std::string>(1, runtime_config))
            .ok());

    core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> metadata =
        service->MetadataStore();
    MT_EXPECT_TRUE(metadata.ok());
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string> leased_referrers =
        metadata.value()->HardCommitHoldReferrers(Hard("sha256:leased"));
    MT_EXPECT_TRUE(leased_referrers.ok());
    MT_EXPECT_TRUE(leased_referrers.value() == std::vector<ImageCacheHoldOwner>(1, owner));
    const char* const excluded[] = {"sha256:outside", "sha256:remote"};
    for (std::size_t i = 0; i < 2; ++i) {
        core::Expected<std::vector<ImageCacheHoldOwner>, std::string> referrers =
            metadata.value()->HardCommitHoldReferrers(Hard(excluded[i]));
        MT_EXPECT_TRUE(referrers.ok());
        MT_EXPECT_TRUE(referrers.value().empty());
    }

    core::Expected<ImageCacheGcReport, std::string> report =
        service->RunGc(ImageCacheLiveRuntimeRefs(), true);
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(core::fs::Exists(leased));
    const std::map<std::string, ImageCacheGcBlockedReason> reasons =
        BlockedReasons(report.value());
    MT_EXPECT_TRUE(ReasonIs(reasons, "sha256:leased", ImageCacheGcBlockedReason::Held));
    MT_EXPECT_TRUE(OwnersOf(reasons, "sha256:leased") ==
                   std::vector<ImageCacheHoldOwner>(1, owner));
}

MT_TEST(runtime_config_hold_fails_closed_for_descriptorless_commit_store_file) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string commit_file =
        WriteCommitFile(*service, "sha256:missing-descriptor", "payload");
    const std::string runtime_config = temp.path + "/runtime/image.json";
    core::Json lower = core::Json(core::JsonObject());
    lower["file"] = commit_file;
    WriteImageConfig(runtime_config, "", Lowers(lower));

    core::Expected<core::Unit, std::string> result = service->AcquireHoldForConfigPaths(
        Owner("runtime", "sandbox-running"), std::vector<std::string>(1, runtime_config));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("commit-store file but no digest") != std::string::npos);
}

MT_TEST(runtime_config_hold_fails_closed_for_sizeless_commit_store_file) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string commit_file = WriteCommitFile(*service, "sha256:missing-size", "payload");
    const std::string runtime_config = temp.path + "/runtime/image.json";
    core::Json lower = core::Json(core::JsonObject());
    lower["file"] = commit_file;
    lower["digest"] = "sha256:missing-size";
    WriteImageConfig(runtime_config, "", Lowers(lower));

    core::Expected<core::Unit, std::string> result = service->AcquireHoldForConfigPaths(
        Owner("runtime", "sandbox-running"), std::vector<std::string>(1, runtime_config));
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(result.error().find("commit-store file but no size") != std::string::npos);
}

MT_TEST(cleanup_stale_runtime_holds_clears_only_runtime_namespaces) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string file = WriteCommitFile(*service, "sha256:held", "held");
    MT_EXPECT_TRUE(service
                       ->RecordHardCommitObject("sha256:held", core::Optional<std::string>(file),
                                                core::Optional<uint64_t>(4),
                                                std::vector<p2p::P2pArtifactKey>())
                       .ok());

    const ImageCacheHoldOwner runtime_owner = Owner("runtime", "sandbox-1");
    const ImageCacheHoldOwner paused_owner = Owner("paused", "sandbox-paused");
    const ImageCacheHoldOwner operation_owner = Owner("operation", "oci-conversion/1");
    const std::string config = service->config_dir() + "/base-image.json";
    WriteImageConfig(config, "", Lowers(Lower(file, "sha256:held", 4)));
    MT_EXPECT_TRUE(service->RecordSourceConfigRefsFromConfigPath(config).ok());
    const ImageCacheHoldOwner owners[] = {runtime_owner, paused_owner, operation_owner};
    for (std::size_t i = 0; i < 3; ++i) {
        MT_EXPECT_TRUE(service
                           ->CreateOrReplaceHold(owners[i], HardRefs(std::vector<std::string>(
                                                                1, "sha256:held")))
                           .ok());
    }

    core::Expected<std::size_t, std::string> released = service->CleanupStaleRuntimeHolds();
    MT_EXPECT_TRUE(released.ok());
    MT_EXPECT_EQ(released.value(), static_cast<std::size_t>(2));

    // Runtime/operation leases gone; the source config root and the paused
    // hold (durable across restart) survive.
    core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string> metadata =
        service->MetadataStore();
    MT_EXPECT_TRUE(metadata.ok());
    core::Expected<std::map<HardCommitId, std::vector<ImageCacheConfigId> >, std::string>
        config_referrers = metadata.value()->HardCommitConfigReferrerMap();
    MT_EXPECT_TRUE(config_referrers.ok());
    MT_EXPECT_TRUE(config_referrers.value()[Hard("sha256:held")] ==
                   std::vector<ImageCacheConfigId>(
                       1, ImageCacheConfigId::FromConfigPath(config).value()));
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string> holders =
        metadata.value()->HardCommitHoldReferrers(Hard("sha256:held"));
    MT_EXPECT_TRUE(holders.ok());
    MT_EXPECT_TRUE(holders.value() == std::vector<ImageCacheHoldOwner>(1, paused_owner));
    core::Expected<std::vector<std::string>, std::string> paused_keys =
        service->ListHoldKeysInNamespace("paused");
    MT_EXPECT_TRUE(paused_keys.ok());
    MT_EXPECT_TRUE(paused_keys.value() == std::vector<std::string>(1, "sandbox-paused"));

    core::Expected<ImageCacheGcReport, std::string> report =
        service->RunGc(ImageCacheLiveRuntimeRefs(), true);
    MT_EXPECT_TRUE(report.ok());
    MT_EXPECT_EQ(report.value().collected, static_cast<std::size_t>(0));
    MT_EXPECT_TRUE(core::fs::Exists(file));
}

MT_TEST(maintenance_sweeps_stale_indexes) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    WriteCommitFile(*service, "sha256:live", "live");
    WriteTestIndex(*service, "sha256:src-live", "sha256:live");
    WriteTestIndex(*service, "sha256:src-dead", "sha256:dead");

    core::Expected<std::size_t, std::string> removed = service->ReclaimStaleIndexes();
    MT_EXPECT_TRUE(removed.ok());
    MT_EXPECT_EQ(removed.value(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(
        core::fs::Exists(service->index_dir() + "/" + DigestSlug("sha256:src-live")));
    MT_EXPECT_TRUE(
        !core::fs::Exists(service->index_dir() + "/" + DigestSlug("sha256:src-dead")));
}

MT_TEST(evict_source_configs_over_capacity_removes_unrooted_config) {
    TempRoot temp;
    std::shared_ptr<ImageCacheService> service = TestService(temp);
    const std::string config_path = service->config_dir() + "/base-image.json";
    WriteConfig(config_path);

    // High/low watermark 0 selects every config; eviction un-roots it by
    // removing the source config file so the hard-commit GC can reclaim it.
    MT_EXPECT_TRUE(service->EvictSourceConfigsOverCapacity(0, 0, 0).ok());
    MT_EXPECT_TRUE(!core::fs::Exists(config_path));
}

int main() { return microtest::RunAll(); }
