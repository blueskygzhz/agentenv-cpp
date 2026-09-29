// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/index.rs — LinearizedBptree / IndexLBPT.
//
// The load-bearing property is not "the tree is a tree" but that IndexLBPT
// returns exactly what the binary-search ReadOnlyIndex returns. Every
// structural test below is therefore paired with a differential check.
#include "agentenv/storage/overlaybd/lsmt.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv::storage::overlaybd::lsmt;  // NOLINT

namespace {

SegmentMapping SM(uint64_t off, uint32_t len, uint64_t moff) {
    return SegmentMapping(off, len, moff, false, 0);
}

/// Contiguous, non-overlapping mappings — the shape a sealed layer has.
std::vector<SegmentMapping> DenseMappings(size_t count, uint32_t len) {
    std::vector<SegmentMapping> v;
    for (size_t i = 0; i < count; ++i) {
        v.push_back(SM(static_cast<uint64_t>(i) * len, len, static_cast<uint64_t>(i) * len));
    }
    return v;
}

/// Mappings with holes, so `search` has to land on a gap sometimes.
std::vector<SegmentMapping> SparseMappings(size_t count, uint32_t len, uint64_t stride) {
    std::vector<SegmentMapping> v;
    for (size_t i = 0; i < count; ++i) {
        v.push_back(SM(static_cast<uint64_t>(i) * stride, len, static_cast<uint64_t>(i) * len));
    }
    return v;
}

bool SameResult(const std::vector<SegmentMapping>& a,
                const std::vector<SegmentMapping>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i])) return false;
    }
    return true;
}

/// IndexLBPT and ReadOnlyIndex must agree for every query in the sweep.
template <typename Cfg>
bool AgreesWithBinarySearch(const std::vector<SegmentMapping>& mappings,
                            uint64_t virtual_size, uint64_t max_offset,
                            uint32_t query_len) {
    IndexLBPT<Cfg> lbpt(mappings, virtual_size);
    ReadOnlyIndex ro(mappings);
    for (uint64_t off = 0; off <= max_offset; ++off) {
        std::vector<SegmentMapping> from_tree, from_binary;
        lbpt.Lookup(Segment(off, query_len), &from_tree);
        ro.Lookup(Segment(off, query_len), &from_binary);
        if (!SameResult(from_tree, from_binary)) return false;
    }
    return true;
}

}  // namespace

// ---- config tables must match Rust exactly; Search indexes them directly ----
MT_TEST(bpt_config_tables_match_rust) {
    MT_EXPECT_TRUE(BptU64::kKeysPerNode == 8);
    MT_EXPECT_TRUE(BptU64::kMaxLevel == 10);
    MT_EXPECT_TRUE(BptU64::NodesPerLevel()[0] == 8);
    MT_EXPECT_TRUE(BptU64::NodesPerLevel()[1] == 72);
    MT_EXPECT_TRUE(BptU64::NodesPerLevel()[2] == 648);
    MT_EXPECT_TRUE(BptU64::NodesPerLevel()[9] == 3099363912ULL);
    MT_EXPECT_TRUE(BptU64::LevelStartId()[0] == 0);
    MT_EXPECT_TRUE(BptU64::LevelStartId()[1] == 8);
    MT_EXPECT_TRUE(BptU64::LevelStartId()[2] == 80);
    MT_EXPECT_TRUE(BptU64::LevelStartId()[9] == 387420488);

    MT_EXPECT_TRUE(BptU32::kKeysPerNode == 16);
    MT_EXPECT_TRUE(BptU32::kMaxLevel == 7);
    MT_EXPECT_TRUE(BptU32::NodesPerLevel()[0] == 16);
    MT_EXPECT_TRUE(BptU32::NodesPerLevel()[6] == 386200304);
    MT_EXPECT_TRUE(BptU32::LevelStartId()[0] == 0);
    MT_EXPECT_TRUE(BptU32::LevelStartId()[6] == 24137568);
}

// Each level's start id is the running sum of the previous capacities.
MT_TEST(bpt_level_tables_are_self_consistent) {
    for (size_t i = 1; i < BptU64::kMaxLevel; ++i) {
        MT_EXPECT_TRUE(BptU64::LevelStartId()[i] ==
                       BptU64::LevelStartId()[i - 1] + BptU64::NodesPerLevel()[i - 1]);
    }
    for (size_t i = 1; i < BptU32::kMaxLevel; ++i) {
        MT_EXPECT_TRUE(BptU32::LevelStartId()[i] ==
                       BptU32::LevelStartId()[i - 1] + BptU32::NodesPerLevel()[i - 1]);
    }
}

// ---- build ----
MT_TEST(build_rejects_empty_mapping) {
    LinearizedBptree<BptU64> tree;
    std::string err;
    MT_EXPECT_TRUE(!tree.Build(std::vector<SegmentMapping>(), &err));
    MT_EXPECT_TRUE(err == "empty mapping");
    // Rust leaves depth at -1, which makes `search` return 0 unconditionally.
    MT_EXPECT_TRUE(tree.depth() == -1);
    MT_EXPECT_TRUE(tree.Search(12345) == 0);
}

MT_TEST(build_picks_shallowest_sufficient_depth) {
    std::string err;
    // <= 8 leaves fit in level 1.
    {
        LinearizedBptree<BptU64> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(8, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 1);
    }
    // 9 leaves need level 2 (capacity 72).
    {
        LinearizedBptree<BptU64> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(9, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 2);
    }
    // 72 still fits level 2; 73 spills to level 3.
    {
        LinearizedBptree<BptU64> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(72, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 2);
    }
    {
        LinearizedBptree<BptU64> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(73, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 3);
    }
    // The 16-key config has a wider first level.
    {
        LinearizedBptree<BptU32> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(16, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 1);
    }
    {
        LinearizedBptree<BptU32> tree;
        MT_EXPECT_TRUE(tree.Build(DenseMappings(17, 4), &err));
        MT_EXPECT_TRUE(tree.depth() == 2);
    }
}

MT_TEST(build_node_array_is_whole_nodes) {
    std::string err;
    LinearizedBptree<BptU64> tree;
    MT_EXPECT_TRUE(tree.Build(DenseMappings(20, 4), &err));
    // Rust rounds the array down to a whole number of nodes.
    MT_EXPECT_TRUE(tree.nodes().size() % BptU64::kKeysPerNode == 0);
    MT_EXPECT_TRUE(!tree.nodes().empty());
}

MT_TEST(build_is_idempotent_on_rebuild) {
    std::string err;
    LinearizedBptree<BptU64> tree;
    MT_EXPECT_TRUE(tree.Build(DenseMappings(30, 4), &err));
    const std::vector<uint64_t> first = tree.nodes();
    MT_EXPECT_TRUE(tree.Build(DenseMappings(30, 4), &err));
    MT_EXPECT_TRUE(tree.nodes() == first);
}

// ---- IndexLBPT vs ReadOnlyIndex: the differential contract ----
MT_TEST(lbpt_matches_binary_search_single_level) {
    // 8 leaves -> depth 1, the degenerate "root is the leaf" case.
    std::vector<SegmentMapping> m = DenseMappings(8, 4);
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, 8 * 4, 8 * 4 + 4, 1));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, 8 * 4, 8 * 4 + 4, 4));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, 8 * 4, 8 * 4 + 4, 9));
}

MT_TEST(lbpt_matches_binary_search_multi_level) {
    // 73 leaves forces depth 3, so Search descends twice.
    std::vector<SegmentMapping> m = DenseMappings(73, 4);
    const uint64_t vsize = 73 * 4;
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, vsize + 8, 1));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, vsize + 8, 4));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, vsize + 8, 16));
}

MT_TEST(lbpt_matches_binary_search_with_holes) {
    // Gaps mean some queries land between mappings.
    std::vector<SegmentMapping> m = SparseMappings(40, 3, 10);
    const uint64_t vsize = 40 * 10;
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, 420, 1));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, 420, 7));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, 420, 25));
}

MT_TEST(lbpt_matches_binary_search_u32_config) {
    std::vector<SegmentMapping> m = SparseMappings(50, 3, 8);
    const uint64_t vsize = 50 * 8;
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU32>(m, vsize, 410, 1));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU32>(m, vsize, 410, 5));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU32>(m, vsize, 410, 20));
}

MT_TEST(lbpt_matches_binary_search_single_mapping) {
    std::vector<SegmentMapping> m;
    m.push_back(SM(10, 5, 100));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, 64, 32, 1));
    MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, 64, 32, 8));
}

// A randomized sweep: the tree's index arithmetic is easy to get subtly wrong
// in ways a hand-picked case misses.
MT_TEST(lbpt_matches_binary_search_randomized) {
    std::srand(0x5eed);
    for (int round = 0; round < 12; ++round) {
        const size_t count = 1 + static_cast<size_t>(std::rand() % 200);
        std::vector<SegmentMapping> m;
        uint64_t offset = static_cast<uint64_t>(std::rand() % 4);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t len = 1 + static_cast<uint32_t>(std::rand() % 6);
            m.push_back(SM(offset, len, offset));
            // Random gap keeps the mappings sorted and non-overlapping.
            offset += len + static_cast<uint64_t>(std::rand() % 4);
        }
        const uint64_t vsize = offset + 16;
        const uint32_t qlen = 1 + static_cast<uint32_t>(std::rand() % 10);
        MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU64>(m, vsize, offset + 8, qlen));
        MT_EXPECT_TRUE(AgreesWithBinarySearch<BptU32>(m, vsize, offset + 8, qlen));
    }
}

// ---- Lookup guard clauses ----
MT_TEST(lbpt_lookup_guards) {
    std::vector<SegmentMapping> m = DenseMappings(20, 4);
    IndexLBPT<BptU64> idx(m, 80);
    std::vector<SegmentMapping> out;

    // Zero-length query reads nothing.
    MT_EXPECT_TRUE(idx.Lookup(Segment(0, 0), &out) == 0);
    MT_EXPECT_TRUE(out.empty());

    // Rust also guards on virtual_size == 0.
    IndexLBPT<BptU64> zero_vsize(m, 0);
    MT_EXPECT_TRUE(zero_vsize.Lookup(Segment(0, 8), &out) == 0);
    MT_EXPECT_TRUE(out.empty());

    // No mappings at all.
    IndexLBPT<BptU64> empty(std::vector<SegmentMapping>(), 80);
    MT_EXPECT_TRUE(empty.Lookup(Segment(0, 8), &out) == 0);
    MT_EXPECT_TRUE(out.empty());
}

MT_TEST(lbpt_lookup_appends_and_returns_delta) {
    std::vector<SegmentMapping> m = DenseMappings(10, 4);
    IndexLBPT<BptU64> idx(m, 40);

    std::vector<SegmentMapping> out;
    out.push_back(SM(999, 1, 999));  // pre-existing entry must survive
    const size_t added = idx.Lookup(Segment(0, 8), &out);
    // Rust returns `dst.len() - start_len`, not `dst.len()`.
    MT_EXPECT_EQ(static_cast<int>(added), 2);
    MT_EXPECT_EQ(static_cast<int>(out.size()), 3);
    MT_EXPECT_TRUE(out[0] == SM(999, 1, 999));
}

MT_TEST(lbpt_lookup_clips_partial_overlaps) {
    std::vector<SegmentMapping> m;
    m.push_back(SM(0, 10, 100));
    m.push_back(SM(10, 10, 200));
    IndexLBPT<BptU64> idx(m, 20);

    std::vector<SegmentMapping> out;
    // Query [5, 15) clips the head of the first and the tail of the second.
    idx.Lookup(Segment(5, 10), &out);
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);
    MT_EXPECT_TRUE(out[0] == SM(5, 5, 105));
    MT_EXPECT_TRUE(out[1] == SM(10, 5, 200));
}

MT_TEST(lbpt_lookup_beyond_last_mapping_is_empty) {
    std::vector<SegmentMapping> m = DenseMappings(10, 4);
    IndexLBPT<BptU64> idx(m, 40);
    std::vector<SegmentMapping> out;
    MT_EXPECT_TRUE(idx.Lookup(Segment(1000, 8), &out) == 0);
    MT_EXPECT_TRUE(out.empty());
}

// A backed zero must keep its physical range through the LBPT path too.
MT_TEST(lbpt_lookup_preserves_backed_zero_offsets) {
    std::vector<SegmentMapping> m;
    m.push_back(SegmentMapping(0, 10, 100, true, 0));
    m.push_back(SegmentMapping(10, 10, kNoPhysicalOffset, true, 0));
    IndexLBPT<BptU64> idx(m, 20);

    std::vector<SegmentMapping> out;
    idx.Lookup(Segment(5, 10), &out);
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);
    // Clipped backed zero advanced its physical cursor.
    MT_EXPECT_TRUE(out[0].HasPhysicalRange());
    MT_EXPECT_TRUE(out[0].moffset == 105);
    // Unbacked zero kept the sentinel.
    MT_EXPECT_TRUE(!out[1].HasPhysicalRange());
    MT_EXPECT_TRUE(out[1].moffset == kNoPhysicalOffset);
}

MT_MAIN
