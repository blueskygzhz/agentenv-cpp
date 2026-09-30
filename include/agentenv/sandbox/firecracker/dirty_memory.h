// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs — converting Firecracker
// dirty memory ranges into overlaybd segment mappings.
//
// This is the load-bearing arithmetic of an incremental memory snapshot, and it
// straddles two different alignments:
//
//   * Firecracker reports dirty memory in 4096-byte pages, and every range it
//     hands out is page-aligned.
//   * overlaybd addresses its layers in 512-byte sectors, so `offset`,
//     `length` and `moffset` are all *sector* counts, not byte counts.
//
// An off-by-one in that division silently writes the wrong guest pages into the
// snapshot, which surfaces much later as a VM that resumes into corrupt memory.
// So every input is validated up front rather than trusted: a page size that is
// not 4096, a range that is not page-aligned, a zero length, or a range that
// runs past the declared memory size are all rejected.
//
// `Segment::kMaxLength` bounds one mapping's sector count, so a long dirty run
// is split into several mappings that stay contiguous in both the destination
// image and the source host address space.
//
// The final overlap check is on *destination* sectors only. Two dirty ranges
// may legitimately share a host virtual address (the same page mapped twice),
// but writing two mappings to one image sector would make the snapshot's
// contents depend on layer ordering.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_DIRTY_MEMORY_H_
#define AGENTENV_SANDBOX_FIRECRACKER_DIRTY_MEMORY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust `FIRECRACKER_DIRTY_PAGE_SIZE` — the only page size Firecracker's
/// dirty-tracking API is accepted with.
const uint64_t kFirecrackerDirtyPageSize = 4096;
/// Rust `OVERLAYBD_ALIGNMENT` — overlaybd's sector size.
const uint64_t kOverlaybdAlignment = 512;
/// Rust `DIRECT_MEMORY_SNAPSHOT_COMPACTION_CONCURRENCY`.
const std::size_t kDirectMemorySnapshotCompactionConcurrency = 32;

/// Rust `firecracker_client::models::DirtyMemoryRange`.
///
/// The fields are signed because the Firecracker API declares them as JSON
/// integers; they are validated into unsigned before any arithmetic.
struct DirtyMemoryRange {
    /// Base host virtual address of the dirty range.
    int64_t base_host_virt_addr = 0;
    /// Cumulative byte offset of this range in the contiguous memory snapshot
    /// image layout. Deliberately *not* a guest physical address.
    int64_t image_offset = 0;
    /// Dirty range length in bytes.
    int64_t length = 0;
};

/// Rust `firecracker_client::models::DirtyMemoryRanges`.
struct DirtyMemoryRanges {
    /// Page size used to interpret dirty bits.
    int32_t page_size = 0;
    /// Total byte size of the contiguous memory snapshot image.
    int64_t memory_size = 0;
    std::vector<DirtyMemoryRange> ranges;
};

/// Rust `checked_i64_to_u64` — the API's signed integers must be non-negative
/// before they can be used as offsets or lengths.
core::Expected<uint64_t, std::string> CheckedI64ToU64(int64_t value,
                                                      const std::string& field);

/// Rust `dirty_ranges_to_segment_mappings`.
///
/// Returns the mappings (sorted by destination offset) and the validated
/// memory size. Fails rather than truncating on any malformed input.
core::Expected<std::vector<storage::overlaybd::lsmt::SegmentMapping>, std::string>
    DirtyRangesToSegmentMappings(const DirtyMemoryRanges& dirty_ranges,
                                 uint64_t* memory_size_out);

/// Rust `split_runtime_suffix`.
///
/// Partitions a layer stack at the first layer that lives under a
/// runtime-owned root. Everything from that point on is runtime-owned,
/// including any later layer that is *not* itself under such a root: the stack
/// is ordered, so a durable layer sitting above a runtime layer still depends
/// on it and cannot be published on its own.
void SplitRuntimeSuffix(const std::vector<std::string>& lower_files,
                        const std::vector<std::string>& runtime_owned_roots,
                        std::vector<std::string>* durable_prefix,
                        std::vector<std::string>* runtime_owned_suffix);

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_DIRTY_MEMORY_H_
