// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs `mod tests` — the
// dirty-page to segment-mapping conversion, plus the alignment and overlap
// edge cases the upstream tests reach only indirectly.
#include "agentenv/sandbox/firecracker/dirty_memory.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;                          // NOLINT
using namespace agentenv::sandbox::firecracker;    // NOLINT

namespace {

namespace obd = agentenv::storage::overlaybd::lsmt;

DirtyMemoryRange Range(int64_t base, int64_t offset, int64_t length) {
    DirtyMemoryRange range;
    range.base_host_virt_addr = base;
    range.image_offset        = offset;
    range.length              = length;
    return range;
}

DirtyMemoryRanges Ranges(int64_t memory_size, const std::vector<DirtyMemoryRange>& ranges,
                         int32_t page_size = static_cast<int32_t>(kFirecrackerDirtyPageSize)) {
    DirtyMemoryRanges out;
    out.page_size   = page_size;
    out.memory_size = memory_size;
    out.ranges      = ranges;
    return out;
}

std::vector<DirtyMemoryRange> One(const DirtyMemoryRange& a) {
    std::vector<DirtyMemoryRange> out;
    out.push_back(a);
    return out;
}

std::vector<DirtyMemoryRange> Two(const DirtyMemoryRange& a, const DirtyMemoryRange& b) {
    std::vector<DirtyMemoryRange> out;
    out.push_back(a);
    out.push_back(b);
    return out;
}

const int64_t kPage = static_cast<int64_t>(kFirecrackerDirtyPageSize);
const int64_t kSector = static_cast<int64_t>(kOverlaybdAlignment);

}  // namespace

// ---- Rust's own tests ------------------------------------------------------

MT_TEST(dirty_ranges_split_large_ranges) {
    // Rust `dirty_ranges_to_segment_mappings_splits_large_ranges`.
    const int64_t first_chunk = static_cast<int64_t>(obd::Segment::kMaxLength) * kSector;
    const DirtyMemoryRanges ranges =
        Ranges(first_chunk + kSector, One(Range(0x1000000, 0, first_chunk + kSector)));

    uint64_t memory_size = 0;
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(ranges, &memory_size);
    MT_EXPECT_TRUE(mappings.ok());

    MT_EXPECT_EQ(memory_size, static_cast<uint64_t>(ranges.memory_size));
    // One mapping cannot exceed kMaxLength sectors, so the run is split.
    MT_EXPECT_EQ(mappings.value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(mappings.value()[0].Offset(), static_cast<uint64_t>(0));
    MT_EXPECT_EQ(mappings.value()[0].Length(), obd::Segment::kMaxLength);
    MT_EXPECT_EQ(mappings.value()[0].moffset, static_cast<uint64_t>(0x1000000) / kOverlaybdAlignment);

    // The split stays contiguous in both the destination image and the source
    // host address space.
    MT_EXPECT_EQ(mappings.value()[1].Offset(),
                 static_cast<uint64_t>(obd::Segment::kMaxLength));
    MT_EXPECT_EQ(mappings.value()[1].Length(), static_cast<uint32_t>(1));
    MT_EXPECT_EQ(mappings.value()[1].moffset,
                 static_cast<uint64_t>(0x1000000) / kOverlaybdAlignment +
                     obd::Segment::kMaxLength);
}

MT_TEST(dirty_ranges_reject_non_4k_page_size) {
    // Rust `dirty_ranges_to_segment_mappings_rejects_non_4k_page_size`.
    // Every alignment check assumes 4096; a different page size would make
    // them all silently wrong.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, std::vector<DirtyMemoryRange>(), 2048), NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("dirty memory page_size must be 4096") !=
                   std::string::npos);
}

MT_TEST(dirty_ranges_reject_a_non_page_aligned_range) {
    // Rust `dirty_ranges_to_segment_mappings_rejects_non_page_aligned_range`.
    // A sector-sized length is sector-aligned but not *page*-aligned, which is
    // exactly the case that would slip through a looser check.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0x1000000, 0, kSector))), NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("dirty range length 512 is not 4096-byte aligned") !=
                   std::string::npos);
}

MT_TEST(dirty_ranges_reject_overlapping_destinations) {
    // Rust `dirty_ranges_to_segment_mappings_rejects_overlaps`. Two mappings
    // landing on one image sector would make the snapshot's contents depend on
    // layer ordering.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(
            Ranges(kPage * 3, Two(Range(0x1000000, 0, kPage * 2),
                                  Range(0x2000000, kPage, kPage))),
            NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("overlapping dirty memory destination ranges detected") !=
                   std::string::npos);
}

// ---- alignment and validation ----------------------------------------------

MT_TEST(dirty_ranges_convert_bytes_to_sectors) {
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage * 2, One(Range(kPage, kPage, kPage))), NULL);
    MT_EXPECT_TRUE(mappings.ok());
    MT_EXPECT_EQ(mappings.value().size(), static_cast<std::size_t>(1));
    // offset/length/moffset are all sector counts, not byte counts: one 4096
    // byte page is 8 sectors of 512.
    MT_EXPECT_EQ(mappings.value()[0].Offset(),
                 static_cast<uint64_t>(kPage) / kOverlaybdAlignment);
    MT_EXPECT_EQ(mappings.value()[0].Length(),
                 static_cast<uint32_t>(kPage / static_cast<int64_t>(kOverlaybdAlignment)));
    MT_EXPECT_EQ(mappings.value()[0].moffset,
                 static_cast<uint64_t>(kPage) / kOverlaybdAlignment);
}

MT_TEST(dirty_ranges_reject_a_misaligned_memory_size) {
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage + 1, std::vector<DirtyMemoryRange>()), NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("memory_size") != std::string::npos);
    MT_EXPECT_TRUE(mappings.error().find("not 4096-byte aligned") != std::string::npos);
}

MT_TEST(dirty_ranges_reject_a_misaligned_base_or_offset) {
    // A misaligned host address.
    MT_EXPECT_TRUE(
        !DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(1, 0, kPage))), NULL).ok());
    // A misaligned destination offset.
    MT_EXPECT_TRUE(
        !DirtyRangesToSegmentMappings(Ranges(kPage * 2, One(Range(0, 1, kPage))), NULL).ok());
}

MT_TEST(dirty_ranges_reject_a_zero_length_range) {
    // A zero-length range carries no pages; accepting it would hide a bug in
    // the reporter rather than surface it.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0, 0, 0))), NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("length must be positive") != std::string::npos);
}

MT_TEST(dirty_ranges_reject_a_range_past_the_memory_size) {
    // A range ending past the declared image would make the snapshot describe
    // memory that does not exist.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0, 0, kPage * 2))), NULL);
    MT_EXPECT_TRUE(!mappings.ok());
    MT_EXPECT_TRUE(mappings.error().find("exceeds memory_size") != std::string::npos);
}

MT_TEST(dirty_ranges_reject_negative_fields) {
    // The Firecracker API declares these as signed JSON integers, so a
    // negative value has to be caught before any unsigned arithmetic.
    MT_EXPECT_TRUE(!DirtyRangesToSegmentMappings(
                        Ranges(-1, std::vector<DirtyMemoryRange>()), NULL)
                        .ok());
    MT_EXPECT_TRUE(
        !DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(-kPage, 0, kPage))), NULL).ok());
    MT_EXPECT_TRUE(
        !DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0, -kPage, kPage))), NULL).ok());
    MT_EXPECT_TRUE(
        !DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0, 0, -kPage))), NULL).ok());
}

MT_TEST(dirty_ranges_accept_adjacent_non_overlapping_destinations) {
    // Touching but not overlapping: previous_end == next.offset is allowed.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(
            Ranges(kPage * 2, Two(Range(0x2000000, kPage, kPage),
                                  Range(0x1000000, 0, kPage))),
            NULL);
    MT_EXPECT_TRUE(mappings.ok());
    MT_EXPECT_EQ(mappings.value().size(), static_cast<std::size_t>(2));
    // Sorted by destination, so the second input range comes first.
    MT_EXPECT_EQ(mappings.value()[0].Offset(), static_cast<uint64_t>(0));
    MT_EXPECT_EQ(mappings.value()[1].Offset(),
                 static_cast<uint64_t>(kPage) / kOverlaybdAlignment);
}

MT_TEST(dirty_ranges_allow_a_shared_source_address) {
    // The overlap check is on *destination* sectors only: the same host page
    // may legitimately back two distinct image offsets.
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(
            Ranges(kPage * 2, Two(Range(0x1000000, 0, kPage),
                                  Range(0x1000000, kPage, kPage))),
            NULL);
    MT_EXPECT_TRUE(mappings.ok());
    MT_EXPECT_EQ(mappings.value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(mappings.value()[0].moffset, mappings.value()[1].moffset);
}

MT_TEST(dirty_ranges_with_no_ranges_produce_no_mappings) {
    uint64_t memory_size = 0;
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, std::vector<DirtyMemoryRange>()),
                                     &memory_size);
    MT_EXPECT_TRUE(mappings.ok());
    MT_EXPECT_TRUE(mappings.value().empty());
    // The memory size is still reported: a VM with no dirty pages still has a
    // memory image.
    MT_EXPECT_EQ(memory_size, static_cast<uint64_t>(kPage));
}

MT_TEST(dirty_ranges_mappings_are_not_zeroed) {
    const core::Expected<std::vector<obd::SegmentMapping>, std::string> mappings =
        DirtyRangesToSegmentMappings(Ranges(kPage, One(Range(0, 0, kPage))), NULL);
    MT_EXPECT_TRUE(mappings.ok());
    // Dirty pages carry real data, so the mapping must own a physical range
    // rather than read as a hole.
    MT_EXPECT_TRUE(!mappings.value()[0].zeroed);
    MT_EXPECT_TRUE(mappings.value()[0].HasPhysicalRange());
}

// ---- checked conversion ----------------------------------------------------

MT_TEST(dirty_checked_i64_to_u64) {
    const core::Expected<uint64_t, std::string> ok = CheckedI64ToU64(42, "field");
    MT_EXPECT_TRUE(ok.ok());
    MT_EXPECT_EQ(ok.value(), static_cast<uint64_t>(42));

    MT_EXPECT_TRUE(CheckedI64ToU64(0, "field").ok());

    const core::Expected<uint64_t, std::string> bad = CheckedI64ToU64(-1, "memory_size");
    MT_EXPECT_TRUE(!bad.ok());
    // The field name is in the message so the operator knows which input was
    // malformed.
    MT_EXPECT_TRUE(bad.error().find("memory_size") != std::string::npos);
    MT_EXPECT_TRUE(bad.error().find("must be non-negative") != std::string::npos);
}

// ---- runtime-owned suffix --------------------------------------------------

MT_TEST(dirty_split_runtime_suffix_partitions_at_the_first_runtime_layer) {
    std::vector<std::string> lowers;
    lowers.push_back("/var/lib/agentenv/durable-1.commit");
    lowers.push_back("/var/lib/agentenv/durable-2.commit");
    lowers.push_back("/run/agentenv/runtime-1.commit");
    lowers.push_back("/run/agentenv/runtime-2.commit");

    std::vector<std::string> roots;
    roots.push_back("/run/agentenv");

    std::vector<std::string> durable;
    std::vector<std::string> runtime;
    SplitRuntimeSuffix(lowers, roots, &durable, &runtime);

    MT_EXPECT_EQ(durable.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(runtime.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(durable[0], std::string("/var/lib/agentenv/durable-1.commit"));
    MT_EXPECT_EQ(runtime[0], std::string("/run/agentenv/runtime-1.commit"));
}

MT_TEST(dirty_split_runtime_suffix_takes_everything_above_the_first_match) {
    std::vector<std::string> lowers;
    lowers.push_back("/var/lib/agentenv/durable.commit");
    lowers.push_back("/run/agentenv/runtime.commit");
    // Not itself runtime-owned, but it sits *above* a runtime layer.
    lowers.push_back("/var/lib/agentenv/later.commit");

    std::vector<std::string> roots;
    roots.push_back("/run/agentenv");

    std::vector<std::string> durable;
    std::vector<std::string> runtime;
    SplitRuntimeSuffix(lowers, roots, &durable, &runtime);

    // The stack is ordered, so a durable layer above a runtime layer still
    // depends on it and cannot be published on its own.
    MT_EXPECT_EQ(durable.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(runtime.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(runtime[1], std::string("/var/lib/agentenv/later.commit"));
}

MT_TEST(dirty_split_runtime_suffix_with_no_match_keeps_everything_durable) {
    std::vector<std::string> lowers;
    lowers.push_back("/var/lib/agentenv/a.commit");
    lowers.push_back("/var/lib/agentenv/b.commit");

    std::vector<std::string> roots;
    roots.push_back("/run/agentenv");

    std::vector<std::string> durable;
    std::vector<std::string> runtime;
    SplitRuntimeSuffix(lowers, roots, &durable, &runtime);
    MT_EXPECT_EQ(durable.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(runtime.empty());
}

MT_TEST(dirty_split_runtime_suffix_uses_component_wise_prefixes) {
    std::vector<std::string> lowers;
    // A sibling directory whose name shares a string prefix with the root is
    // *not* inside it; a raw string prefix would misclassify this as runtime
    // owned and refuse to publish a durable layer.
    lowers.push_back("/run/agentenv-extra/a.commit");

    std::vector<std::string> roots;
    roots.push_back("/run/agentenv");

    std::vector<std::string> durable;
    std::vector<std::string> runtime;
    SplitRuntimeSuffix(lowers, roots, &durable, &runtime);
    MT_EXPECT_EQ(durable.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(runtime.empty());
}

MT_TEST(dirty_split_runtime_suffix_handles_an_empty_stack) {
    std::vector<std::string> durable;
    std::vector<std::string> runtime;
    SplitRuntimeSuffix(std::vector<std::string>(), std::vector<std::string>(), &durable,
                       &runtime);
    MT_EXPECT_TRUE(durable.empty());
    MT_EXPECT_TRUE(runtime.empty());
}

int main() { return microtest::RunAll(); }
