// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs —
//   rewrite_lowers_with_runtime_roots, build_mem_snapshot_image_config,
//   local_layer_config, load_existing_image_config.
//
// This module had no tests at all, which mattered because it is the thing that
// keeps a sandbox resumable: every pause appends one memory layer, so without
// the budget and the compaction below the stack grows by one per pause until it
// hits overlaybd's 255-layer hard limit and the sandbox can never be resumed
// again. The two decisions worth pinning down are *when* the runtime-owned
// suffix gets compacted instead of adopted, and *which* layers count as
// runtime-owned in the first place -- a layer misclassified as durable leaves
// the snapshot referencing a file the runtime deletes.
#include "agentenv/sandbox/firecracker/mem_image_config.h"

#include <cstdio>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;                        // NOLINT
using namespace agentenv::sandbox::firecracker;  // NOLINT

namespace {

namespace obd = agentenv::storage::overlaybd;

obd::LayerConfig Lower(const std::string& file) {
    obd::LayerConfig layer;
    layer.file = file;
    return layer;
}

// A lower that lives in the repository: no local file, only a digest.
obd::LayerConfig RemoteLower(const std::string& digest) {
    obd::LayerConfig layer;
    layer.digest = digest;
    layer.repo_blob_url = "https://registry.example/v2/repo/blobs";
    return layer;
}

// Records what was asked of it; no real overlaybd layers involved.
struct Recorder {
    std::vector<std::string> adopted;
    std::vector<std::string> compacted;
    std::vector<std::size_t> compacted_counts;
    bool compact_returns_nothing = false;
    bool compact_fails = false;
    bool adopt_fails = false;

    LayerAdopter Adopt() {
        Recorder* self = this;
        return [self](const std::string& source,
                      const std::string& destination) -> core::Expected<core::Unit, std::string> {
            self->adopted.push_back(source + " -> " + destination);
            if (self->adopt_fails) return core::make_unexpected(std::string("adoption refused"));
            return core::Unit();
        };
    }

    LayerCompactor Compact() {
        Recorder* self = this;
        return [self](const std::vector<obd::LayerConfig>& layers, const std::string& output_path)
                   -> core::Expected<core::Optional<std::string>, std::string> {
            self->compacted.push_back(output_path);
            self->compacted_counts.push_back(layers.size());
            if (self->compact_fails) {
                return core::make_unexpected(std::string("compaction refused"));
            }
            if (self->compact_returns_nothing) return core::Optional<std::string>();
            return core::Optional<std::string>(output_path);
        };
    }
};

const char* const kRuntimeRoot = "/run/aenv/managed-snapshots";

std::vector<std::string> RuntimeRoots() {
    std::vector<std::string> roots;
    roots.push_back(kRuntimeRoot);
    return roots;
}

// `count` runtime-owned lowers under the runtime root.
std::vector<obd::LayerConfig> RuntimeLowers(std::size_t count) {
    std::vector<obd::LayerConfig> out;
    for (std::size_t i = 0; i < count; ++i) {
        char name[128];
        std::snprintf(name, sizeof(name), "%s/layer-%zu.commit", kRuntimeRoot, i);
        out.push_back(Lower(name));
    }
    return out;
}

std::vector<obd::LayerConfig> DurableLowers(std::size_t count) {
    std::vector<obd::LayerConfig> out;
    for (std::size_t i = 0; i < count; ++i) {
        char name[128];
        std::snprintf(name, sizeof(name), "/var/lib/aenv/durable-%zu.commit", i);
        out.push_back(Lower(name));
    }
    return out;
}

std::vector<obd::LayerConfig> Concat(const std::vector<obd::LayerConfig>& a,
                                     const std::vector<obd::LayerConfig>& b) {
    std::vector<obd::LayerConfig> out = a;
    for (std::size_t i = 0; i < b.size(); ++i) out.push_back(b[i]);
    return out;
}

core::Optional<obd::LayerConfig> Appended(const std::string& file) {
    return core::Optional<obd::LayerConfig>(Lower(file));
}

core::Optional<obd::LayerConfig> NoAppended() { return core::Optional<obd::LayerConfig>(); }

}  // namespace

// ---- LocalLayerConfig ------------------------------------------------------

MT_TEST(mem_local_layer_claims_no_digest_or_size) {
    // `digest` and `size` describe a *published* blob. A layer that exists
    // only on this node has neither, and filling them in would make it look
    // fetchable from the repository -- so a restore on another node would try
    // to pull a blob that was never uploaded.
    const obd::LayerConfig layer = LocalLayerConfig("/run/aenv/a.commit");
    MT_EXPECT_EQ(layer.file, std::string("/run/aenv/a.commit"));
    MT_EXPECT_TRUE(layer.digest.empty());
    MT_EXPECT_EQ(layer.size, static_cast<uint64_t>(0));
    MT_EXPECT_TRUE(layer.repo_blob_url.empty());
}

// ---- MaxRuntimeOwnedLayers -------------------------------------------------

MT_TEST(mem_layer_budget_is_full_with_an_empty_prefix) {
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(0), kDefaultMaxOverlaybdSnapshotLayers);
}

MT_TEST(mem_layer_budget_shrinks_by_one_per_four_prefix_layers) {
    // The point is to keep the *total* stack under overlaybd's 255-layer hard
    // limit. A budget that ignored the prefix would let prefix+suffix exceed
    // it while each half looked reasonable.
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(4), kDefaultMaxOverlaybdSnapshotLayers - 1);
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(32), kDefaultMaxOverlaybdSnapshotLayers - 8);
    // Not yet a full step: 3/4 == 0.
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(3), kDefaultMaxOverlaybdSnapshotLayers);
}

MT_TEST(mem_layer_budget_clamps_to_one) {
    // At 0 a lone layer would be compacted into itself on every pause,
    // forever: the budget would always be exceeded by the single appended
    // layer, so each pause would rewrite the whole stack for no gain.
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(1000), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(MaxRuntimeOwnedLayers(128), static_cast<std::size_t>(1));
}

// ---- RewriteLowersWithRuntimeRoots: the adopt path ------------------------

MT_TEST(mem_rewrite_adopts_a_runtime_suffix_under_budget) {
    Recorder recorder;
    const std::vector<obd::LayerConfig> lowers = Concat(DurableLowers(2), RuntimeLowers(2));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    // Nothing compacted: two runtime layers are well under the budget.
    MT_EXPECT_TRUE(recorder.compacted.empty());
    MT_EXPECT_EQ(recorder.adopted.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(4));
}

MT_TEST(mem_rewrite_leaves_the_durable_prefix_untouched) {
    // A durable layer is already in a place the runtime will not delete, so
    // copying it into the snapshot dir would duplicate it for nothing.
    Recorder recorder;
    const std::vector<obd::LayerConfig> lowers = Concat(DurableLowers(2), RuntimeLowers(1));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value()[0].file, std::string("/var/lib/aenv/durable-0.commit"));
    MT_EXPECT_EQ(out.value()[1].file, std::string("/var/lib/aenv/durable-1.commit"));
    MT_EXPECT_EQ(recorder.adopted.size(), static_cast<std::size_t>(1));
}

MT_TEST(mem_rewrite_renames_adopted_layers_with_a_stack_index) {
    // The index keeps stack order explicit in the directory, and two
    // inherited layers may legitimately share a basename -- without the index
    // the second would overwrite the first and the stack would silently lose a
    // layer.
    Recorder recorder;
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower(std::string(kRuntimeRoot) + "/a/layer.commit"));
    lowers.push_back(Lower(std::string(kRuntimeRoot) + "/b/layer.commit"));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value()[0].file,
                 std::string("/snap/out/") + kInheritedLayersDir + "/0000/layer.commit");
    MT_EXPECT_EQ(out.value()[1].file,
                 std::string("/snap/out/") + kInheritedLayersDir + "/0001/layer.commit");
}

MT_TEST(mem_rewrite_appends_the_new_layer_above_the_adopted_suffix) {
    // Order is the whole meaning of a layer stack: the appended layer holds
    // this pause's writes, so it has to end up newest. Putting it lower would
    // let an older layer shadow the newest memory.
    Recorder recorder;
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(2), "/snap/out",
                                      Appended("/snap/out/new.commit"), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(out.value().back().file, std::string("/snap/out/new.commit"));
}

MT_TEST(mem_rewrite_propagates_an_adoption_failure) {
    Recorder recorder;
    recorder.adopt_fails = true;
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(1), "/snap/out", NoAppended(),
                                      "compacted.commit", RuntimeRoots(), recorder.Compact(),
                                      recorder.Adopt());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("adoption refused") != std::string::npos);
}

MT_TEST(mem_rewrite_with_no_runtime_layers_needs_no_adopter) {
    // Everything durable: the adopter is never called, so a caller that has
    // none is not an error. This is the `read_only` snapshot's shape.
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(DurableLowers(3), "/snap/out", NoAppended(),
                                      "compacted.commit", RuntimeRoots(), LayerCompactor(),
                                      LayerAdopter());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(3));
}

MT_TEST(mem_rewrite_without_an_adopter_refuses_rather_than_skipping) {
    // Silently leaving the suffix pointing at runtime files would produce a
    // snapshot that looks complete and breaks the moment the runtime cleans
    // up, which is far worse than a refused capture.
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(1), "/snap/out", NoAppended(),
                                      "compacted.commit", RuntimeRoots(), LayerCompactor(),
                                      LayerAdopter());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("no layer adopter") != std::string::npos);
}

// ---- RewriteLowersWithRuntimeRoots: the compact path ----------------------

MT_TEST(mem_rewrite_compacts_once_the_suffix_exceeds_the_budget) {
    Recorder recorder;
    // Budget with an empty prefix is `kDefaultMaxOverlaybdSnapshotLayers`;
    // one more than that forces compaction.
    const std::vector<obd::LayerConfig> lowers =
        RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers + 1);

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(recorder.compacted.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(recorder.compacted[0], std::string("/snap/out/compacted.commit"));
    // Adoption is the *other* branch; it must not also run.
    MT_EXPECT_TRUE(recorder.adopted.empty());
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(1));
}

MT_TEST(mem_rewrite_counts_the_appended_layer_against_the_budget) {
    // Exactly at the budget, the appended layer is what tips it over. Counting
    // only the existing suffix would let the stack grow one past the limit
    // every time.
    Recorder recorder;
    const std::vector<obd::LayerConfig> lowers =
        RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers);

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", Appended("/snap/out/new.commit"),
                                      "compacted.commit", RuntimeRoots(), recorder.Compact(),
                                      recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(recorder.compacted.size(), static_cast<std::size_t>(1));
    // The appended layer goes into the compaction input, newest last.
    MT_EXPECT_EQ(recorder.compacted_counts[0], kDefaultMaxOverlaybdSnapshotLayers + 1);
}

MT_TEST(mem_rewrite_compacts_at_the_tightened_budget) {
    // A large durable prefix shrinks the budget, so a suffix that would be
    // adopted with no prefix gets compacted here. This is the interaction the
    // budget exists for.
    Recorder recorder;
    const std::size_t prefix = 32;  // budget becomes 32 - 8 == 24
    const std::vector<obd::LayerConfig> lowers =
        Concat(DurableLowers(prefix), RuntimeLowers(25));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(recorder.compacted.size(), static_cast<std::size_t>(1));
    // The durable prefix survives, plus the one compacted layer.
    MT_EXPECT_EQ(out.value().size(), prefix + 1);
}

MT_TEST(mem_rewrite_refuses_to_compact_a_suffix_holding_a_remote_lower) {
    // A lower with an empty `file` lives in the repository, so there is
    // nothing local to read. Compacting anyway would write the hole as zeros
    // and produce a layer that reads as valid and is silently empty.
    Recorder recorder;
    std::vector<obd::LayerConfig> lowers =
        RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers);
    lowers.push_back(RemoteLower("sha256:beef"));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("remote lowers") != std::string::npos);
    // And it refused *before* calling the compactor.
    MT_EXPECT_TRUE(recorder.compacted.empty());
}

MT_TEST(mem_rewrite_keeps_the_prefix_when_there_was_nothing_to_compact) {
    // Rust pushes the compacted layer only when the compactor returns `Some`.
    // Appending an empty layer instead would put a zero-length lower on top of
    // the stack, shadowing real data below it.
    Recorder recorder;
    recorder.compact_returns_nothing = true;
    const std::vector<obd::LayerConfig> lowers =
        Concat(DurableLowers(2), RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers + 1));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(2));
}

MT_TEST(mem_rewrite_propagates_a_compaction_failure) {
    Recorder recorder;
    recorder.compact_fails = true;
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers + 1),
                                      "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("compaction refused") != std::string::npos);
}

MT_TEST(mem_rewrite_without_a_compactor_refuses_rather_than_overflowing) {
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers + 1),
                                      "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), LayerCompactor(), LayerAdopter());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("no layer compactor") != std::string::npos);
}

// ---- runtime-owned classification -----------------------------------------

MT_TEST(mem_rewrite_treats_a_durable_layer_above_a_runtime_one_as_runtime_owned) {
    // The stack is ordered, so a durable layer sitting on a runtime layer
    // still depends on it. Adopting only the genuinely-runtime tail would
    // either leave a dangling reference or change lower precedence.
    Recorder recorder;
    std::vector<obd::LayerConfig> lowers;
    lowers.push_back(Lower("/var/lib/aenv/durable.commit"));
    lowers.push_back(Lower(std::string(kRuntimeRoot) + "/runtime.commit"));
    lowers.push_back(Lower("/var/lib/aenv/later.commit"));

    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(lowers, "/snap/out", NoAppended(), "compacted.commit",
                                      RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    // Both of the top two were adopted, not just the runtime-rooted one.
    MT_EXPECT_EQ(recorder.adopted.size(), static_cast<std::size_t>(2));
}

MT_TEST(mem_rewrite_with_no_runtime_roots_adopts_nothing) {
    // No roots configured means nothing is runtime-owned. Worth pinning: the
    // production roots come from config, and an empty list must degrade to
    // "everything durable" rather than "everything runtime".
    Recorder recorder;
    const core::Expected<std::vector<obd::LayerConfig>, std::string> out =
        RewriteLowersWithRuntimeRoots(RuntimeLowers(3), "/snap/out", NoAppended(),
                                      "compacted.commit", std::vector<std::string>(),
                                      recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(recorder.adopted.empty());
    MT_EXPECT_EQ(out.value().size(), static_cast<std::size_t>(3));
}

// ---- BuildMemSnapshotImageConfig ------------------------------------------

MT_TEST(mem_build_from_a_fresh_launch_starts_from_an_empty_stack) {
    // Rust `load_existing_image_config(None, ..)` yields a default config, so
    // a first pause is not an error.
    Recorder recorder;
    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(), "/snap/out/mem.commit", "/snap/out", RuntimeRoots(),
        recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().lowers.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(out.value().lowers[0].file, std::string("/snap/out/mem.commit"));
}

MT_TEST(mem_build_appends_the_new_memory_layer_newest) {
    Recorder recorder;
    obd::ImageConfig inherited;
    inherited.lowers = DurableLowers(2);

    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(inherited), "/snap/out/mem.commit", "/snap/out",
        RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().lowers.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(out.value().lowers.back().file, std::string("/snap/out/mem.commit"));
}

MT_TEST(mem_build_carries_the_repo_blob_url_across) {
    // The inherited lowers may still reference repository blobs; dropping the
    // base URL would leave them unresolvable on restore.
    Recorder recorder;
    obd::ImageConfig inherited;
    inherited.repo_blob_url = "https://registry.example/v2/repo/blobs";
    inherited.lowers.push_back(RemoteLower("sha256:cafe"));

    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(inherited), "/snap/out/mem.commit", "/snap/out",
        RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(out.value().repo_blob_url,
                 std::string("https://registry.example/v2/repo/blobs"));
}

MT_TEST(mem_build_drops_the_inherited_upper_and_result_file) {
    // Everything but `repo_blob_url` and the lowers describes the *previous*
    // device. Carrying the upper across would have the new snapshot share a
    // writable file with the device it was captured from.
    Recorder recorder;
    obd::ImageConfig inherited;
    inherited.lowers = DurableLowers(1);
    inherited.upper.data = "/run/aenv/live/upper.data";
    inherited.upper.index = "/run/aenv/live/upper.index";
    inherited.result_file = "/run/aenv/live/result.txt";
    inherited.acceleration_layer = true;
    inherited.record_trace_path = "/run/aenv/live/trace";

    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(inherited), "/snap/out/mem.commit", "/snap/out",
        RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(out.value().upper.data.empty());
    MT_EXPECT_TRUE(out.value().upper.index.empty());
    MT_EXPECT_TRUE(out.value().result_file.empty());
    MT_EXPECT_TRUE(!out.value().acceleration_layer);
    MT_EXPECT_TRUE(out.value().record_trace_path.empty());
}

MT_TEST(mem_build_compacts_into_the_memory_output_name) {
    // The name is distinct from the rootfs one (`managed-base.commit`): both
    // land in the same snapshot directory on some layouts, and one name would
    // have the two compactions overwrite each other.
    Recorder recorder;
    obd::ImageConfig inherited;
    inherited.lowers = RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers);

    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(inherited), "/snap/out/mem.commit", "/snap/out",
        RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_EQ(recorder.compacted.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(recorder.compacted[0], std::string("/snap/out/mem_compacted.commit"));
}

MT_TEST(mem_build_propagates_a_rewrite_failure) {
    Recorder recorder;
    recorder.compact_fails = true;
    obd::ImageConfig inherited;
    inherited.lowers = RuntimeLowers(kDefaultMaxOverlaybdSnapshotLayers);

    const core::Expected<obd::ImageConfig, std::string> out = BuildMemSnapshotImageConfig(
        core::Optional<obd::ImageConfig>(inherited), "/snap/out/mem.commit", "/snap/out",
        RuntimeRoots(), recorder.Compact(), recorder.Adopt());
    MT_EXPECT_TRUE(!out.ok());
    MT_EXPECT_TRUE(out.error().find("compaction refused") != std::string::npos);
}

int main() { return microtest::RunAll(); }
