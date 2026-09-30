// SPDX-License-Identifier: MIT
// Rust: src/snapshot/p2p.rs `mod tests`.
#include "agentenv/snapshot/p2p.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/overlaybd/p2p/artifact.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/layer.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "microtest.h"

using namespace agentenv;            // NOLINT
using namespace agentenv::snapshot;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-snapshot-p2p-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-snapshot-p2p-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

/// Writes a minimal sealed overlaybd layer carrying `uuid`.
///
/// Rust's test drives `create_file_rw` + `close_seal`; that writer is not
/// ported, so the layout is assembled directly: a header block, one data
/// block, and a trailer block whose final `kSpace` bytes are what
/// `ReadOverlaybdLayerUuid` reads.
void WriteSealedLayer(const std::string& path, const std::string& uuid) {
    using storage::overlaybd::lsmt::HeaderTrailer;
    const size_t block = static_cast<size_t>(HeaderTrailer::kSpace);

    std::vector<uint8_t> image(block * 2, 0);

    HeaderTrailer header = HeaderTrailer::New();
    header.SetHeader();
    header.SetDataFile();
    header.SetSealed();
    header.virtual_size = block;
    uint8_t header_bytes[HeaderTrailer::kOnDiskSize];
    header.Serialize(header_bytes);
    for (size_t i = 0; i < sizeof(header_bytes); ++i) image[i] = header_bytes[i];

    // Data block: arbitrary non-zero bytes so the file is not sparse.
    for (size_t i = 0; i < block; ++i) image[block + i] = 0x5a;

    HeaderTrailer trailer = HeaderTrailer::New();
    trailer.SetHeader();
    trailer.SetDataFile();
    trailer.SetSealed();
    trailer.SetTrailer();
    trailer.virtual_size = block;
    trailer.index_offset = block * 2;
    trailer.index_size   = 0;
    // The uuid field is a 37-byte NUL-padded ASCII buffer.
    for (size_t i = 0; i < sizeof(trailer.uuid); ++i) trailer.uuid[i] = 0;
    for (size_t i = 0; i < uuid.size() && i < sizeof(trailer.uuid) - 1; ++i) {
        trailer.uuid[i] = static_cast<uint8_t>(uuid[i]);
    }
    uint8_t trailer_bytes[HeaderTrailer::kOnDiskSize];
    trailer.Serialize(trailer_bytes);

    image.resize(block * 3, 0);
    for (size_t i = 0; i < sizeof(trailer_bytes); ++i) {
        image[block * 2 + i] = trailer_bytes[i];
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    MT_EXPECT_TRUE(file != NULL);
    if (file) {
        std::fwrite(&image[0], 1, image.size(), file);
        std::fclose(file);
    }
}

struct Lower {
    std::string file;
    std::string digest;
    uint64_t    size = 0;
};

void WriteImageConfig(const std::string& path, const std::vector<Lower>& lowers) {
    storage::overlaybd::ImageConfig config;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        storage::overlaybd::LayerConfig layer;
        layer.file   = lowers[i].file;
        layer.digest = lowers[i].digest;
        layer.size   = lowers[i].size;
        config.lowers.push_back(layer);
    }
    MT_EXPECT_TRUE(
        core::fs::Write(path, storage::overlaybd::ImageConfigToJson(config)).ok());
}

bool HasKey(const std::vector<SnapshotP2pArtifact>& artifacts,
            const std::string& key) {
    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        if (artifacts[i].key == key) return true;
    }
    return false;
}

const char* const kDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char* const kDigestB =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

}  // namespace

MT_TEST(snapshot_p2p_fixed_artifact_key_shape) {
    auto id = core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());
    MT_EXPECT_EQ(FixedArtifactKey(id.value(), "vm_state.bin"),
                 std::string("snapshot/v1/artifacts/"
                             "11111111-2222-3333-4444-555555555555/vm_state.bin"));
}

MT_TEST(snapshot_p2p_fixed_and_bytes_artifacts) {
    auto id = core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());

    SnapshotP2pArtifact file_artifact =
        SnapshotP2pArtifact::Fixed(id.value(), "manifest.json", "/tmp/manifest.json");
    MT_EXPECT_TRUE(file_artifact.source.kind == p2p::P2pPublishSource::Path);
    MT_EXPECT_EQ(file_artifact.source.path, std::string("/tmp/manifest.json"));
    MT_EXPECT_TRUE(file_artifact.publish_mode == p2p::P2pPublishMode::Copy);
    // Rust uses `Value::Null` for fixed artifacts, i.e. no layer metadata.
    MT_EXPECT_TRUE(file_artifact.metadata.empty());

    std::vector<uint8_t> body;
    body.push_back('h');
    body.push_back('i');
    SnapshotP2pArtifact bytes_artifact =
        SnapshotP2pArtifact::Bytes(id.value(), "inline", body);
    MT_EXPECT_TRUE(bytes_artifact.source.kind == p2p::P2pPublishSource::Bytes);
    MT_EXPECT_EQ(bytes_artifact.source.bytes.size(), static_cast<std::size_t>(2));
}

MT_TEST(snapshot_p2p_layer_artifact_keys_are_namespaced) {
    SnapshotP2pArtifact digest_keyed =
        SnapshotP2pArtifact::ContentAddressedOverlaybdLayer("/tmp/a.commit", kDigestA, 9);
    MT_EXPECT_EQ(digest_keyed.key, overlaybd::p2p::LayerKeyFromDigest(kDigestA));
    MT_EXPECT_TRUE(!digest_keyed.metadata.empty());

    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    SnapshotP2pArtifact uuid_keyed =
        SnapshotP2pArtifact::UuidOverlaybdLayer("/tmp/b.commit", uuid, 4096);
    MT_EXPECT_EQ(uuid_keyed.key, overlaybd::p2p::LayerKeyFromUuid(uuid));
    // A uuid key must never collide with a digest key.
    MT_EXPECT_TRUE(uuid_keyed.key != digest_keyed.key);
}

// Rust `local_overlaybd_layers_publish_only_digest_layers_without_committed_uuid`.
MT_TEST(snapshot_p2p_local_layers_digest_only_without_committed_uuid) {
    TempRoot root;
    const std::string descriptorless = root.path + "/snapshot.commit";
    const std::string described      = root.path + "/described.commit";
    WriteSealedLayer(descriptorless, "11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(core::fs::Write(described, "described").ok());

    std::vector<Lower> lowers;
    Lower first;
    first.file = descriptorless;  // no digest: not publishable by digest
    lowers.push_back(first);
    Lower second;
    second.file   = described;
    second.digest = kDigestA;
    second.size   = 9;
    lowers.push_back(second);

    const std::string config_path = root.path + "/image.json";
    WriteImageConfig(config_path, lowers);

    std::set<std::string> committed_digests;
    committed_digests.insert(kDigestA);

    std::vector<SnapshotP2pArtifact> artifacts =
        SnapshotP2pArtifact::LocalOverlaybdLayers(config_path, committed_digests,
                                                  std::set<std::string>());

    MT_EXPECT_EQ(artifacts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(artifacts[0].publish_mode == p2p::P2pPublishMode::Copy);
    MT_EXPECT_EQ(artifacts[0].key, overlaybd::p2p::LayerKeyFromDigest(kDigestA));
}

// Rust `local_overlaybd_layers_skips_digest_absent_from_committed_record`.
//
// Publish-time compression recontainerizes a raw local layer before upload, so
// the committed record names the compressed digest. Publishing the raw
// descriptor digest would create a key no consumer looks up. uuid-keyed
// publication is unaffected by that guard.
MT_TEST(snapshot_p2p_local_layers_skip_digest_absent_from_committed_record) {
    TempRoot root;
    const std::string layer_path = root.path + "/snapshot.commit";
    const std::string uuid       = "44444444-5555-6666-7777-888888888888";
    WriteSealedLayer(layer_path, uuid);

    std::vector<Lower> lowers;
    Lower only;
    only.file   = layer_path;
    only.digest = kDigestA;  // the raw local digest
    only.size   = 4096;
    lowers.push_back(only);

    const std::string config_path = root.path + "/image.json";
    WriteImageConfig(config_path, lowers);

    // The committed record references the recontainerized bytes instead.
    std::set<std::string> committed_digests;
    committed_digests.insert(kDigestB);
    std::set<std::string> committed_uuids;
    committed_uuids.insert(uuid);

    std::vector<SnapshotP2pArtifact> artifacts =
        SnapshotP2pArtifact::LocalOverlaybdLayers(config_path, committed_digests,
                                                  committed_uuids);

    MT_EXPECT_EQ(artifacts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(artifacts[0].key, overlaybd::p2p::LayerKeyFromUuid(uuid));
    MT_EXPECT_TRUE(!HasKey(artifacts, overlaybd::p2p::LayerKeyFromDigest(kDigestA)));
}

// Rust `local_overlaybd_layers_publishes_uuid_alongside_digest_layers`.
MT_TEST(snapshot_p2p_local_layers_publish_uuid_alongside_digest) {
    TempRoot root;
    const std::string committed_path = root.path + "/snapshot.commit";
    const std::string skipped_path   = root.path + "/skipped.commit";
    const std::string committed_uuid = "22222222-3333-4444-5555-666666666666";
    const std::string skipped_uuid   = "33333333-4444-5555-6666-777777777777";
    WriteSealedLayer(committed_path, committed_uuid);
    WriteSealedLayer(skipped_path, skipped_uuid);

    std::vector<Lower> lowers;
    Lower tracked;
    tracked.file   = committed_path;
    tracked.digest = kDigestA;
    tracked.size   = 4096;
    lowers.push_back(tracked);
    Lower skipped;
    skipped.file = skipped_path;  // uuid not in the committed set
    lowers.push_back(skipped);

    const std::string config_path = root.path + "/image.json";
    WriteImageConfig(config_path, lowers);

    std::set<std::string> committed_digests;
    committed_digests.insert(kDigestA);
    std::set<std::string> committed_uuids;
    committed_uuids.insert(committed_uuid);

    std::vector<SnapshotP2pArtifact> artifacts =
        SnapshotP2pArtifact::LocalOverlaybdLayers(config_path, committed_digests,
                                                  committed_uuids);

    // The tracked layer is published twice — once per key kind — and the
    // untracked one not at all.
    MT_EXPECT_EQ(artifacts.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(HasKey(artifacts, overlaybd::p2p::LayerKeyFromDigest(kDigestA)));
    MT_EXPECT_TRUE(HasKey(artifacts, overlaybd::p2p::LayerKeyFromUuid(committed_uuid)));
    MT_EXPECT_TRUE(!HasKey(artifacts, overlaybd::p2p::LayerKeyFromUuid(skipped_uuid)));
}

MT_TEST(snapshot_p2p_local_layers_skip_dir_only_layers) {
    TempRoot root;
    // A `dir=`-only layer is remote-recoverable: there is no local file to
    // serve, so it must not produce an artifact.
    const std::string config_path = root.path + "/image.json";
    WriteImageConfig(config_path, std::vector<Lower>());

    std::set<std::string> digests;
    digests.insert(kDigestA);
    std::vector<SnapshotP2pArtifact> artifacts =
        SnapshotP2pArtifact::LocalOverlaybdLayers(config_path, digests,
                                                  std::set<std::string>());
    MT_EXPECT_TRUE(artifacts.empty());
}

MT_TEST(snapshot_p2p_local_layers_unreadable_config_is_skipped_not_fatal) {
    TempRoot root;
    // P2P publication is an optional acceleration path, so a broken config is
    // logged and skipped rather than failing the snapshot publish.
    const std::string missing = root.path + "/absent.json";
    MT_EXPECT_TRUE(SnapshotP2pArtifact::LocalOverlaybdLayers(
                       missing, std::set<std::string>(), std::set<std::string>()).empty());

    const std::string malformed = root.path + "/bad.json";
    MT_EXPECT_TRUE(core::fs::Write(malformed, "{not json").ok());
    MT_EXPECT_TRUE(SnapshotP2pArtifact::LocalOverlaybdLayers(
                       malformed, std::set<std::string>(),
                       std::set<std::string>()).empty());
}

MT_TEST(snapshot_p2p_local_layers_skip_nil_uuid_layer) {
    TempRoot root;
    const std::string layer_path = root.path + "/nil.commit";
    // An all-NUL uuid field reads back as nil, which Rust refuses to publish.
    WriteSealedLayer(layer_path, std::string());

    std::vector<Lower> lowers;
    Lower only;
    only.file = layer_path;
    lowers.push_back(only);
    const std::string config_path = root.path + "/image.json";
    WriteImageConfig(config_path, lowers);

    std::set<std::string> committed_uuids;
    committed_uuids.insert("22222222-3333-4444-5555-666666666666");

    MT_EXPECT_TRUE(SnapshotP2pArtifact::LocalOverlaybdLayers(
                       config_path, std::set<std::string>(), committed_uuids).empty());
}

MT_TEST(snapshot_p2p_read_layer_uuid_round_trip) {
    TempRoot root;
    const std::string layer_path = root.path + "/a.commit";
    const std::string uuid       = "11111111-2222-3333-4444-555555555555";
    WriteSealedLayer(layer_path, uuid);

    auto read = storage::overlaybd::ReadOverlaybdLayerUuid(layer_path);
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_EQ(read.value(), uuid);

    auto size = storage::overlaybd::ReadOverlaybdLayerVirtualSize(layer_path);
    MT_EXPECT_TRUE(size.ok());
    MT_EXPECT_EQ(size.value(),
                 static_cast<uint64_t>(storage::overlaybd::lsmt::HeaderTrailer::kSpace));
}

MT_TEST(snapshot_p2p_read_layer_uuid_rejects_unsealed_and_short_files) {
    TempRoot root;
    // Too small to hold a trailer.
    const std::string tiny = root.path + "/tiny.commit";
    MT_EXPECT_TRUE(core::fs::Write(tiny, "x").ok());
    MT_EXPECT_TRUE(!storage::overlaybd::ReadOverlaybdLayerUuid(tiny).ok());

    // Right size, but no valid trailer magic.
    const std::string garbage = root.path + "/garbage.commit";
    std::string filler(
        static_cast<size_t>(storage::overlaybd::lsmt::HeaderTrailer::kSpace) * 2, '\0');
    MT_EXPECT_TRUE(core::fs::Write(garbage, filler).ok());
    MT_EXPECT_TRUE(!storage::overlaybd::ReadOverlaybdLayerUuid(garbage).ok());

    MT_EXPECT_TRUE(
        !storage::overlaybd::ReadOverlaybdLayerUuid(root.path + "/absent.commit").ok());
}

MT_TEST(snapshot_p2p_read_layer_uuid_maps_malformed_uuid_to_nil) {
    TempRoot root;
    const std::string layer_path = root.path + "/bad-uuid.commit";
    // Rust's `Uuid::parse_str(..).unwrap_or_else(|_| Uuid::nil())` turns a
    // non-uuid string into nil; this port represents nil as empty.
    WriteSealedLayer(layer_path, "not-a-uuid");

    auto read = storage::overlaybd::ReadOverlaybdLayerUuid(layer_path);
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(read.value().empty());
}

MT_TEST(snapshot_p2p_fetch_from_disabled_transport_reports_not_found) {
    std::shared_ptr<p2p::P2pTransport> transport(new p2p::DisabledP2pTransport());
    // A disabled transport's lookup yields Ok(None), which `fetch_artifact`
    // must turn into a "not found" error rather than a silent success.
    auto fetched = FetchArtifact(transport, "snapshot/v1/artifacts/x/y", "/tmp/out.bin");
    MT_EXPECT_TRUE(!fetched.ok());

    auto bytes = FetchArtifactBytes(transport, "snapshot/v1/artifacts/x/y");
    MT_EXPECT_TRUE(!bytes.ok());
}

MT_TEST(snapshot_p2p_publish_to_disabled_transport_succeeds) {
    auto id = core::SnapshotId::Parse("11111111-2222-3333-4444-555555555555");
    MT_EXPECT_TRUE(id.ok());
    std::shared_ptr<p2p::P2pTransport> transport(new p2p::DisabledP2pTransport());

    // Publishing is an optional acceleration path, so a disabled transport
    // accepts it as a no-op.
    MT_EXPECT_TRUE(SnapshotP2pArtifact::Fixed(id.value(), "vm_state.bin", "/tmp/vm")
                       .Publish(transport).ok());
}

int main() { return microtest::RunAll(); }
