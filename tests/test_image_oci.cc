// SPDX-License-Identifier: MIT
// Tests for image OCI classification — ported 1:1 from oci_image.rs #[cfg(test)].
#include "microtest.h"

#include "agentenv/image/oci_image.h"

using namespace agentenv::image;

static OciLayerDescriptor layer(const std::string& mt, const std::string& digest) {
  OciLayerDescriptor l;
    l.media_type = mt;
  l.digest = digest;
    return l;
}

// Rust: classify_layer_flags_overlaybd_native_media_types
MT_TEST(classify_layer_native_media_types) {
    MT_EXPECT_TRUE(ClassifyLayer(layer(
        "application/vnd.containerd.overlaybd.image.layer.v1.zfile", "sha256:aaa"))
        == LayerClass::OverlaybdNative);
    MT_EXPECT_TRUE(ClassifyLayer(layer(
        "application/vnd.containerd.overlaybd.v1.tar", "sha256:aaa"))
  == LayerClass::OverlaybdNative);
}

// Rust: classify_layer_recognizes_standard_tar_types
MT_TEST(classify_layer_standard_tar) {
    const char* mts[] = {
    "application/vnd.oci.image.layer.v1.tar",
      "application/vnd.oci.image.layer.v1.tar+gzip",
  "application/vnd.oci.image.layer.v1.tar+zstd",
    "application/vnd.docker.image.rootfs.diff.tar",
        "application/vnd.docker.image.rootfs.diff.tar.gzip",
    };
for (size_t i = 0; i < sizeof(mts) / sizeof(mts[0]); ++i) {
      MT_EXPECT_TRUE(ClassifyLayer(layer(mts[i], "sha256:aaa")) == LayerClass::StandardTar);
    }
}

// Rust: classify_layer_treats_tar_with_self_referential_overlaybd_annotation_as_native
MT_TEST(classify_layer_self_referential_native) {
    std::string digest = "sha256:4cafc55d878a0f1f2fb497369b138272eda40a8c66b7c922f0693b89cff6b0f0";
    OciLayerDescriptor l = layer("application/vnd.oci.image.layer.v1.tar", digest);
    l.annotations["containerd.io/snapshot/overlaybd/blob-digest"] = digest;
    l.annotations["containerd.io/snapshot/overlaybd/blob-size"] = "47760418";
    MT_EXPECT_TRUE(ClassifyLayer(l) == LayerClass::OverlaybdNative);
}

// Rust: classify_layer_flags_tar_wrapped_overlaybd_when_annotation_points_at_inner_blob
MT_TEST(classify_layer_tar_wrapped) {
    OciLayerDescriptor l = layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:outerbeef");
    l.annotations["containerd.io/snapshot/overlaybd/blob-digest"] = "sha256:innerbeef";
    MT_EXPECT_TRUE(ClassifyLayer(l) == LayerClass::OverlaybdTarWrapped);
}

// Rust: classify_layer_returns_unknown_for_unrecognized_media_types
MT_TEST(classify_layer_unknown) {
    MT_EXPECT_TRUE(ClassifyLayer(layer("application/vnd.something.exotic.v1", "sha256:aaa"))
        == LayerClass::Unknown);
}

MT_TEST(classify_layer_turbo_annotation) {
    OciLayerDescriptor l = layer("application/vnd.oci.image.layer.v1.tar", "sha256:aaa");
    l.annotations["containerd.io/snapshot/overlaybd/turbo-oci/target-media-type"] = "x";
    MT_EXPECT_TRUE(ClassifyLayer(l) == LayerClass::OverlaybdTurbo);
}

// Rust: classify_manifest_recognizes_standard_oci_image
MT_TEST(classify_manifest_standard_oci) {
    OciManifest m;
    m.layers.push_back(layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:a"));
    m.layers.push_back(layer("application/vnd.oci.image.layer.v1.tar+zstd", "sha256:b"));
    m.layers.push_back(layer("application/vnd.docker.image.rootfs.diff.tar.gzip", "sha256:c"));
 auto r = ClassifyManifest(m);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value() == ImageFormat::StandardOci);
}

// Rust: classify_manifest_recognizes_overlaybd_native_image
MT_TEST(classify_manifest_overlaybd_native) {
    OciManifest m;
    m.layers.push_back(layer("application/vnd.containerd.overlaybd.image.layer.v1.zfile", "sha256:a"));
    m.layers.push_back(layer("application/vnd.containerd.overlaybd.image.layer.v1.zfile", "sha256:b"));
    auto r = ClassifyManifest(m);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value() == ImageFormat::OverlaybdNative);
}

// Rust: classify_manifest_rejects_mixed_standard_and_overlaybd_layers
MT_TEST(classify_manifest_rejects_mixed) {
    OciManifest m;
    m.layers.push_back(layer("application/vnd.oci.image.layer.v1.tar+gzip", "sha256:a"));
    m.layers.push_back(layer("application/vnd.containerd.overlaybd.image.layer.v1.zfile", "sha256:b"));
    auto r = ClassifyManifest(m);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("mix of standard OCI and overlaybd") != std::string::npos);
}

MT_TEST(classify_manifest_rejects_empty) {
    OciManifest m;
    MT_EXPECT_TRUE(!ClassifyManifest(m).ok());
}

MT_TEST(classify_manifest_rejects_turbo_artifact_type) {
    OciManifest m;
    m.has_artifact_type = true;
    m.artifact_type = "application/vnd.containerd.overlaybd.turbo.v1+json";
    m.layers.push_back(layer("application/vnd.oci.image.layer.v1.tar", "sha256:a"));
    auto r = ClassifyManifest(m);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("turbo") != std::string::npos);
}

MT_MAIN
