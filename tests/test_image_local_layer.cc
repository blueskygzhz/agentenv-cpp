// SPDX-License-Identifier: MIT
// Rust: src/image/local_layer.rs — `mod tests`.
//
// `runtime_generated_delta_names` is ported case for case. The two negative
// names in it are the whole point of the test: the predicate decides whether a
// file on this node is treated as published image content, so a substring or
// suffix match would hand a half-written temp file to the loader.
#include "microtest.h"

#include <string>

#include "agentenv/image/local_layer.h"

using namespace agentenv;
using namespace agentenv::image;

MT_TEST(runtime_generated_delta_names) {
    MT_EXPECT_TRUE(RootfsLayerIsRuntimeGeneratedDelta("/a/b/snapshot.commit"));
    MT_EXPECT_TRUE(RootfsLayerIsRuntimeGeneratedDelta("/a/b/managed-base.commit"));
    // Still recognized although it is no longer produced: layers published
    // while the capture-time compression switches existed are still out there,
    // and dropping the name would make those repositories unreadable.
    MT_EXPECT_TRUE(RootfsLayerIsRuntimeGeneratedDelta("/a/b/snapshot.zfile.commit"));
    // A different layer file, not runtime-generated.
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta("/a/b/overlaybd.commit"));
    // The one that matters: an interrupted write leaves `.tmp` behind, and a
    // suffix or substring match would publish a partially written file.
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta("/a/b/snapshot.commit.tmp"));
}

MT_TEST(runtime_generated_delta_matches_on_the_file_name_only) {
    // No Rust counterpart; pins the `file_name()` semantics the predicate is
    // built on. A *directory* named like a layer is not one, and a path with
    // no file name at all must answer false rather than crash.
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta("/a/snapshot.commit/data"));
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta("/"));
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta(""));
    // A bare name with no directory is still a match: the predicate is about
    // the name, not about where it sits.
    MT_EXPECT_TRUE(RootfsLayerIsRuntimeGeneratedDelta("snapshot.commit"));
    // Prefixing the name does not make it a layer either.
    MT_EXPECT_TRUE(!RootfsLayerIsRuntimeGeneratedDelta("/a/b/old-snapshot.commit"));
}

MT_TEST(layer_config_from_local_layer_sets_only_file_digest_and_size) {
    LocalLayer layer;
    layer.path = "/var/lib/agentenv/layers/abc/snapshot.commit";
    layer.digest = "sha256:abc";
    layer.size = 47760418;

    const storage::overlaybd::LayerConfig config = LayerConfigFromLocalLayer(layer);
    MT_EXPECT_TRUE(config.file == layer.path);
    MT_EXPECT_TRUE(config.digest == layer.digest);
    MT_EXPECT_TRUE(config.size == layer.size);

    // Everything else keeps its default, which is what `..Default::default()`
    // means upstream. `dir` in particular must stay empty: a layer named by
    // `file` that also names a directory gives the loader two candidate
    // locations for the same layer.
    MT_EXPECT_TRUE(config.dir.empty());
    MT_EXPECT_TRUE(config.gzip_index.empty());
    MT_EXPECT_TRUE(config.target_file.empty());
    MT_EXPECT_TRUE(config.target_digest.empty());
    MT_EXPECT_TRUE(config.uuid.empty());
    // Empty rather than a placeholder: `EffectiveRepoBlobUrl` falls back to the
    // image-level URL exactly when this is empty, and a local layer has no
    // remote blob to point at.
    MT_EXPECT_TRUE(config.repo_blob_url.empty());
}

MT_TEST(local_layer_equality_covers_every_field) {
    LocalLayer base;
    base.path = "/layers/a/snapshot.commit";
    base.digest = "sha256:a";
    base.size = 10;

    LocalLayer same = base;
    MT_EXPECT_TRUE(same == base);

    // Rust `#[derive(PartialEq, Eq)]` compares all three. Size is included
    // because two layers may share a path and digest across a truncation.
    LocalLayer other = base;
    other.size = 11;
    MT_EXPECT_TRUE(other != base);
    other = base;
    other.digest = "sha256:b";
    MT_EXPECT_TRUE(other != base);
    other = base;
    other.path = "/layers/b/snapshot.commit";
    MT_EXPECT_TRUE(other != base);
}

MT_MAIN
