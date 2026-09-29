// SPDX-License-Identifier: MIT
// Rust: src/snapshot/repository/backends/posixfs/artifacts.rs `mod tests`.
#include "agentenv/snapshot/repository/posixfs/artifacts.h"

#include <sys/stat.h>

#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/repository/posixfs/layout.h"
#include "agentenv/snapshot/types.h"
#include "microtest.h"

using namespace agentenv;                                    // NOLINT
using namespace agentenv::snapshot;                          // NOLINT
using namespace agentenv::snapshot::repository;              // NOLINT
using namespace agentenv::snapshot::repository::posixfs;     // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        auto d = core::fs::CreateTempDir("agentenv-artifacts-");
        path = d.ok() ? d.value() : std::string("/tmp/agentenv-artifacts-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

void WriteFile(const std::string& path, const std::string& body) {
    core::Optional<std::string> parent = core::fs::Parent(path);
    if (parent.has_value()) core::fs::CreateDirAll(*parent);
    core::fs::Write(path, body);
}

bool SameInode(const std::string& a, const std::string& b) {
    struct stat sa, sb;
    if (::stat(a.c_str(), &sa) != 0) return false;
    if (::stat(b.c_str(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

}  // namespace

// ---- Rust: derives_external_layers_from_layer_repo_blob_urls ----
MT_TEST(derives_external_layers_from_layer_repo_blob_urls) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);

    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\n"
              "  \"repoBlobUrl\": \"\",\n"
              "  \"lowers\": [\n"
              "    { \"digest\": \"sha256:base\", \"size\": 4096,"
              "      \"repoBlobUrl\": \"https://registry.example/v2/ns/image/blobs\" },\n"
              "    { \"digest\": \"sha256:delta\", \"size\": 8192,"
              "      \"repoBlobUrl\": \"s3://bucket/prefix/managed-layers\" }\n"
              "  ],\n"
              "  \"upper\": {},\n"
              "  \"resultFile\": \"\"\n"
              "}\n");

    auto layers = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(layers.ok());
    MT_EXPECT_EQ(static_cast<int>(layers.value().size()), 2);

    MT_EXPECT_TRUE(layers.value()[0].is_external());
    MT_EXPECT_TRUE(layers.value()[0].external.digest == "sha256:base");
    MT_EXPECT_TRUE(layers.value()[0].external.repo_blob_url ==
                   "https://registry.example/v2/ns/image/blobs");
    MT_EXPECT_TRUE(layers.value()[0].external.size == 4096);

    MT_EXPECT_TRUE(layers.value()[1].is_external());
    MT_EXPECT_TRUE(layers.value()[1].external.digest == "sha256:delta");
    MT_EXPECT_TRUE(layers.value()[1].external.repo_blob_url ==
                   "s3://bucket/prefix/managed-layers");
    MT_EXPECT_TRUE(layers.value()[1].external.size == 8192);
}

// The image-level repoBlobUrl is the fallback when the layer omits one.
MT_TEST(external_layer_inherits_image_repo_blob_url) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"https://registry.example/base\","
              "\"lowers\":[{\"digest\":\"sha256:a\",\"size\":1}],"
              "\"upper\":{},\"resultFile\":\"\"}");

    auto layers = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(layers.ok());
    MT_EXPECT_TRUE(layers.value()[0].external.repo_blob_url == "https://registry.example/base");
}

// Rust's digest fallback chain: digest -> target_digest -> "external:{index}".
MT_TEST(external_layer_digest_fallback_chain) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"https://r/x\","
              "\"lowers\":["
              "{\"targetDigest\":\"sha256:target-only\",\"size\":2},"
              "{\"size\":3}"
              "],\"upper\":{},\"resultFile\":\"\"}");

    auto layers = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(layers.ok());
    MT_EXPECT_EQ(static_cast<int>(layers.value().size()), 2);
    MT_EXPECT_TRUE(layers.value()[0].external.digest == "sha256:target-only");
    // Index 1 has neither digest, so it gets the positional synthetic name.
    MT_EXPECT_TRUE(layers.value()[1].external.digest == "external:1");
}

MT_TEST(layer_without_file_or_repo_blob_url_is_unsupported) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"\",\"lowers\":[{\"digest\":\"sha256:x\",\"size\":1}],"
              "\"upper\":{},\"resultFile\":\"\"}");

    auto layers = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(!layers.ok());
    MT_EXPECT_TRUE(layers.error().kind == RepositoryErrorKind::Unsupported);
}

// ---- descriptor-backed import trusts the descriptor instead of re-hashing ----
MT_TEST(import_uses_declared_local_layer_descriptor) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);

    const std::string lower = core::fs::Join(root.path, "rootfs.commit");
    WriteFile(lower, "0123456789");  // exactly 10 bytes

    // Deliberately not the content digest.
    const std::string declared = "sha256:declared-rootfs";
    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + lower +
                  "\",\"digest\":\"" + declared + "\",\"size\":10}],"
                  "\"upper\":{},\"resultFile\":\"\"}");

    auto layers = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(layers.ok());
    MT_EXPECT_EQ(static_cast<int>(layers.value().size()), 1);
    MT_EXPECT_TRUE(layers.value()[0].is_managed());
    MT_EXPECT_TRUE(layers.value()[0].managed.digest == declared);
    MT_EXPECT_TRUE(layers.value()[0].managed.size == 10);
    MT_EXPECT_TRUE(core::fs::Exists(store.ManagedLayerPath(declared)));
}

// A descriptor whose size disagrees with the file is rejected: it is the one
// cheap invariant the descriptor path does validate.
MT_TEST(import_rejects_descriptor_size_mismatch) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string lower = core::fs::Join(root.path, "layer.commit");
    WriteFile(lower, "0123456789");

    auto imported = store.ImportManagedLayerWithDescriptor(lower, "sha256:d", 99);
    MT_EXPECT_TRUE(!imported.ok());
    MT_EXPECT_TRUE(imported.error().kind == RepositoryErrorKind::Backend);
}

// ---- descriptorless lowers: rejected for rootfs, allowed for volumes ----
MT_TEST(descriptorless_lower_rejected_unless_allowed) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string lower = core::fs::Join(root.path, "delta.commit");
    WriteFile(lower, "delta-bytes");
    const std::string image = core::fs::Join(root.path, "image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"\",\"lowers\":[{\"file\":\"" + lower + "\"}],"
              "\"upper\":{},\"resultFile\":\"\"}");

    auto rejected = store.DeriveRootfsLayers(image, false);
    MT_EXPECT_TRUE(!rejected.ok());
    MT_EXPECT_TRUE(rejected.error().kind == RepositoryErrorKind::Unsupported);

    // publish_volume_backing passes allow_descriptorless = true.
    auto allowed = store.PublishVolumeBacking(image);
    MT_EXPECT_TRUE(allowed.ok());
    MT_EXPECT_EQ(static_cast<int>(allowed.value().size()), 1);
    MT_EXPECT_TRUE(allowed.value()[0].is_managed());
    // The digest is the file's real content hash.
    auto expected = core::DescribeFile(lower);
    MT_EXPECT_TRUE(expected.ok());
    MT_EXPECT_TRUE(allowed.value()[0].managed.digest == expected.value().sha256);
    MT_EXPECT_TRUE(allowed.value()[0].managed.size == expected.value().size);
}

// ---- managed layers are content-addressed and immutable ----
MT_TEST(store_managed_layer_is_idempotent) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string source = core::fs::Join(root.path, "layer.commit");
    WriteFile(source, "abcdef");

    auto first = store.ImportManagedLayerByHash(source);
    MT_EXPECT_TRUE(first.ok());
    auto second = store.ImportManagedLayerByHash(source);
    MT_EXPECT_TRUE(second.ok());
    MT_EXPECT_TRUE(first.value().digest == second.value().digest);
    MT_EXPECT_TRUE(first.value().size == second.value().size);
}

MT_TEST(store_managed_layer_rejects_size_mismatch_on_existing_digest) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string source = core::fs::Join(root.path, "layer.commit");
    WriteFile(source, "abcdef");  // 6 bytes

    auto stored = store.StoreManagedLayer(source, "sha256:fixed", 6,
                                          core::Optional<std::string>());
    MT_EXPECT_TRUE(stored.ok());

    // Same digest, different declared size: must not silently overwrite.
    auto conflict = store.StoreManagedLayer(source, "sha256:fixed", 7,
                                            core::Optional<std::string>());
    MT_EXPECT_TRUE(!conflict.ok());
    MT_EXPECT_TRUE(conflict.error().kind == RepositoryErrorKind::Backend);
}

MT_TEST(store_managed_layer_hard_links_when_possible) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string source = core::fs::Join(root.path, "layer.commit");
    WriteFile(source, "link-me");

    auto stored = store.ImportManagedLayerByHash(source);
    MT_EXPECT_TRUE(stored.ok());
    // Same filesystem, so the import must not have copied the bytes.
    MT_EXPECT_TRUE(SameInode(source, store.ManagedLayerPath(stored.value().digest)));
}

MT_TEST(managed_layer_is_made_read_only) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string source = core::fs::Join(root.path, "layer.commit");
    WriteFile(source, "ro");

    auto stored = store.ImportManagedLayerByHash(source);
    MT_EXPECT_TRUE(stored.ok());
    struct stat st;
    MT_EXPECT_TRUE(::stat(store.ManagedLayerPath(stored.value().digest).c_str(), &st) == 0);
    // mode & !0o222 clears every write bit.
    MT_EXPECT_TRUE((st.st_mode & 0222) == 0);
}

// ---- memory layers must be local; there is nowhere external for them ----
MT_TEST(memory_layer_without_file_is_unsupported) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string image = core::fs::Join(root.path, "mem_image.json");
    WriteFile(image,
              "{\"repoBlobUrl\":\"https://r/x\","
              "\"lowers\":[{\"digest\":\"sha256:a\",\"size\":1}],"
              "\"upper\":{},\"resultFile\":\"\"}");

    auto layers = store.DeriveMemoryLayers(image);
    MT_EXPECT_TRUE(!layers.ok());
    MT_EXPECT_TRUE(layers.error().kind == RepositoryErrorKind::Unsupported);
}

MT_TEST(memory_layer_without_descriptor_is_hashed) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string lower = core::fs::Join(root.path, "mem.commit");
    WriteFile(lower, "memory-bytes");
    const std::string image = core::fs::Join(root.path, "mem_image.json");
    // Production mem image configs carry no digest/size for a freshly written
    // lower, so the import must hash the physical file.
    WriteFile(image, "{\"lowers\":[{\"file\":\"" + lower + "\"}]}");

    auto layers = store.DeriveMemoryLayers(image);
    MT_EXPECT_TRUE(layers.ok());
    MT_EXPECT_EQ(static_cast<int>(layers.value().size()), 1);
    auto expected = core::DescribeFile(lower);
    MT_EXPECT_TRUE(layers.value()[0].digest == expected.value().sha256);
    MT_EXPECT_TRUE(layers.value()[0].size == expected.value().size);
}

// ---- copy_local_artifact ----
MT_TEST(copy_local_artifact_hard_links_and_verifies_size) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    const std::string source = core::fs::Join(root.path, "vm_state.bin");
    WriteFile(source, "vm-state-bytes");
    const std::string destination =
        core::fs::Join(core::fs::Join(root.path, "snap"), "vm_state.bin");

    auto copied = store.CopyLocalArtifact(destination, source);
    MT_EXPECT_TRUE(copied.ok());
    MT_EXPECT_TRUE(core::fs::Exists(destination));
    MT_EXPECT_TRUE(SameInode(source, destination));

    // Re-running is a no-op: same_file short-circuits before touching bytes.
    auto again = store.CopyLocalArtifact(destination, source);
    MT_EXPECT_TRUE(again.ok());
}

MT_TEST(copy_local_artifact_fails_for_missing_source) {
    TempRoot root;
    PosixFsArtifactStore store(root.path);
    auto copied = store.CopyLocalArtifact(core::fs::Join(root.path, "dst"),
                                          core::fs::Join(root.path, "nope"));
    MT_EXPECT_TRUE(!copied.ok());
}

// ---- helpers ----
MT_TEST(same_file_reports_false_for_missing_destination) {
    TempRoot root;
    const std::string source = core::fs::Join(root.path, "a");
    WriteFile(source, "x");
    auto r = SameFile(source, core::fs::Join(root.path, "missing"));
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value());
}

MT_TEST(copy_file_with_sha256_round_trips) {
    TempRoot root;
    const std::string source = core::fs::Join(root.path, "src");
    const std::string destination = core::fs::Join(root.path, "dst");
    WriteFile(source, "payload-to-verify");

    auto copied = CopyFileWithSha256(source, destination);
    MT_EXPECT_TRUE(copied.ok());
    auto a = core::DescribeFile(source);
    auto b = core::DescribeFile(destination);
    MT_EXPECT_TRUE(a.ok() && b.ok());
    MT_EXPECT_TRUE(a.value() == b.value());
}

// ---- the artifact layout table is an on-disk ABI ----
MT_TEST(snapshot_artifact_layout_names_match_rust) {
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.firecracker_manifest) ==
                   "firecracker-manifest.json");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.vm_state) == "vm_state.bin");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.memory_dump) == "mem.bin");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.memory_image_config) == "mem_image.json");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.rootfs_dir) == "rootfs");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.drives_dir) == "drives");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.drive_layers_dir) == "drives/layers");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.rootfs_image_config) ==
                   "rootfs/image.json");
    MT_EXPECT_TRUE(std::string(kSnapshotArtifactLayout.overlaybd_image_config_file) ==
                   "image.json");
}

MT_MAIN
