// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/pack_planner.rs — physical block planner for the
// startup manifest prefetch.
//
// Maps an ordered first-touch *logical* page list onto 256 KiB-aligned blocks
// of the final (post-publish) memory layer objects.
//
// All of it is read-only metadata work: merged-index lookups, zfile jump-table
// extent translation, and an optional tar base offset. Nothing here
// decompresses a page, re-reads a payload, or touches the network — the caller
// supplies the final local layer bytes.
#ifndef AGENTENV_STORAGE_OVERLAYBD_PACK_PLANNER_H_
#define AGENTENV_STORAGE_OVERLAYBD_PACK_PLANNER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `OBJECT_BLOCK_BYTES` — cache-block granularity of the final object,
/// and of the prefetch refills that will fill it.
const uint64_t kObjectBlockBytes = 256ull << 10;

/// Rust `PLAN_PAGE_BYTES` — guest page granularity of the recorded
/// first-touch list.
const uint64_t kPlanPageBytes = 4096;

/// Rust `SECTOR_BYTES` — LSMT index units. Mappings are in sectors, so every
/// offset and length coming out of a lookup has to be scaled by this.
const uint64_t kPlanSectorBytes = 512;

/// Rust's budget model. These describe the *v2 artifact* the plan will be
/// written into, not the plan in memory: the budget bounds the file a
/// consumer has to download, so the planner has to account for the framing
/// it will acquire.
const uint64_t kPlanHeaderBytes = 64;
/// Per-block entry: object index u16 + block id u32 + reserved u16.
const uint64_t kPlanEntryBytes = 8;
/// Object table entry: digest length u16 + digest bytes + size u64.
const uint64_t kPlanObjectEntryFixedBytes = 10;
/// Verify-frame granularity; each frame costs a 4-byte checksum.
const uint64_t kPlanFrameBytes = 1ull << 20;

/// Rust `FinalLayerBytes` — where a layer's bytes come from.
struct FinalLayerBytes {
    enum Kind {
        /// A plain local path, opened read-only by the planner.
        kLocalPath,
        /// An already-open handle, e.g. a cache-only view of a remote
        /// object: reads go through whatever backs it.
        kVFile
    };

    Kind kind;
    std::string path;
    std::shared_ptr<VirtualFile> file;

    FinalLayerBytes() : kind(kLocalPath) {}

    static FinalLayerBytes OfLocalPath(const std::string& p) {
        FinalLayerBytes out;
        out.kind = kLocalPath;
        out.path = p;
        return out;
    }
    static FinalLayerBytes OfVFile(std::shared_ptr<VirtualFile> f) {
        FinalLayerBytes out;
        out.kind = kVFile;
        out.file = f;
        return out;
    }
};

/// Rust `FinalLayerSource` — the exact bytes that were (or will be) uploaded,
/// with their content identity.
struct FinalLayerSource {
    std::string digest;
    uint64_t size;
    FinalLayerBytes source;

    FinalLayerSource() : size(0) {}
};

/// Rust `PlannedBlock`. `block_id` counts in `kObjectBlockBytes` units of the
/// object named by `object`.
struct PlannedBlock {
    uint16_t object;
    uint32_t block_id;
    /// True for a layer's metadata blocks (header + index region), false for
    /// data. Kept distinct because a consumer may want the metadata first
    /// even when it trims the data tail.
    bool metadata;

    PlannedBlock() : object(0), block_id(0), metadata(false) {}
};

/// Rust `PlannedObject`.
struct PlannedObject {
    std::string digest;
    uint64_t size;
    /// Index into the caller's `layers` vector this object came from.
    size_t layer;

    PlannedObject() : size(0), layer(0) {}
};

/// Rust `PlanStats`.
struct PlanStats {
    size_t pages_total;
    size_t pages_backed;
    /// Pages that resolved to no physical range at all -- a hole, or past the
    /// end of every layer. Not an error: an untouched page of a sparse memory
    /// image reads as zero.
    size_t pages_zero_hole;
    size_t data_blocks;
    size_t metadata_blocks;
    size_t unique_blocks;
    uint64_t payload_bytes;
    /// Pages whose blocks did not fit the remaining budget. The plan stops at
    /// the first such page, so every later page is counted here too.
    size_t pages_truncated_by_budget;
    /// Missing-block runs in the *complement* of the selection -- what the
    /// background download still has to fetch once this plan is imported.
    size_t gap_runs;
    size_t gap_blocks;

    PlanStats()
        : pages_total(0), pages_backed(0), pages_zero_hole(0), data_blocks(0),
          metadata_blocks(0), unique_blocks(0), payload_bytes(0),
          pages_truncated_by_budget(0), gap_runs(0), gap_blocks(0) {}
};

/// Rust `PackPlan`.
struct PackPlan {
    std::vector<PlannedObject> objects;
    /// First-need order, unique per `(object, block_id)`.
    std::vector<PlannedBlock> blocks;
    PlanStats stats;
};

/// Rust `PageObserver` — per-page resolution callback.
///
/// Invoked once per *processed* page with that page's resolved blocks and
/// their positions in `PackPlan::blocks` (empty for a hole). A block already
/// selected by an earlier page keeps the position of its first selection, so
/// positions always describe first-need order -- which is what lets a
/// consumer map "page X is ready" onto "slots 3, 7 have landed".
class PageObserver {
 public:
    virtual ~PageObserver() {}

    struct Resolved {
        uint16_t object;
        uint32_t block_id;
        size_t position;
    };

    virtual void OnPage(uint32_t page, const std::vector<Resolved>& blocks) = 0;
};

/// Rust `plan_blocks` — plan for an ordered list of 4 KiB page numbers.
///
/// `budget_bytes` bounds the *total v2 file size*, not the payload: header,
/// object table, entry table, frame checksums and slot payload all count. The
/// plan stops at the first page whose newly required blocks would exceed it.
core::Expected<PackPlan, std::string> PlanBlocks(const std::vector<uint32_t>& pages,
                                                    uint64_t mem_virtual_size,
                                                    const std::vector<FinalLayerSource>& layers,
                                                    uint64_t budget_bytes);

/// Rust `plan_blocks_with_observer`.
core::Expected<PackPlan, std::string> PlanBlocksWithObserver(
    const std::vector<uint32_t>& pages, uint64_t mem_virtual_size,
    const std::vector<FinalLayerSource>& layers, uint64_t budget_bytes,
    PageObserver* on_page);

/// Rust `plan_ranges` — one merged-index lookup per span instead of per page.
///
/// Spans must be page-aligned and within the image. Blocks come out in
/// first-need order following the input span order, deduplicated and
/// budget-checked per span exactly as the per-page path does -- the only
/// difference is lookup count, which matters when a trace covers megabytes.
core::Expected<PackPlan, std::string> PlanRanges(
    const std::vector<std::pair<uint64_t, uint64_t> >& ranges, uint64_t mem_virtual_size,
    const std::vector<FinalLayerSource>& layers, uint64_t budget_bytes);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_PACK_PLANNER_H_
