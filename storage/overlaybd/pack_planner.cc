// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/pack_planner.rs
#include "agentenv/storage/overlaybd/pack_planner.h"

#include <algorithm>
#include <sstream>

#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/tar.h"
#include "agentenv/storage/overlaybd/zfile.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

uint64_t DivCeil(uint64_t a, uint64_t b) { return b == 0 ? 0 : (a + b - 1) / b; }

/// Rust `LayerLayout` — how a logical layer offset becomes a physical one.
struct LayerLayout {
    enum Kind {
        /// Raw LSMT bytes: logical == physical, plus the tar base.
        kRaw,
        /// ZFile-compressed: every logical block has to go through the jump
        /// table, because the compressed extents are not uniform.
        kZFile
    };

    Kind kind;
    std::shared_ptr<zfile::ZFileRO> zfile;
    /// Offset of the layer's payload inside its tar wrapper, or 0. Added to
    /// every physical offset: the object's blocks are counted from the start
    /// of the *tar*, which is what was uploaded.
    uint64_t tar_base;

    LayerLayout() : kind(kRaw), tar_base(0) {}
};

struct OpenedLayer {
    std::shared_ptr<VirtualFile> lsmt_file;
    LayerLayout layout;
    ReadOnlyLayerMetadata metadata;
};

/// Rust `open_layer` — unwrap tar, detect zfile, load the layer's metadata.
core::Expected<OpenedLayer, std::string> OpenLayer(const FinalLayerSource& source) {
    std::shared_ptr<VirtualFile> local;
    if (source.source.kind == FinalLayerBytes::kLocalPath) {
        const core::Expected<std::shared_ptr<LocalFile>, std::string> opened =
            LocalFile::OpenRo(source.source.path);
        if (!opened.ok()) {
            return core::make_unexpected("open layer " + source.source.path + ": " +
                                         opened.error());
        }
        local = opened.value();
    } else {
        if (!source.source.file) {
            return core::make_unexpected(std::string("final layer vfile is null"));
        }
        local = source.source.file;
    }

    // Tar first: a layer may be shipped inside a tar, and both the zfile
    // probe and every extent below have to see the payload, not the wrapper.
    //
    // `tar_base` stays zero here. Rust adds the archive's base offset to
    // every physical offset, but this port's `NewTarFileAdaptor` does not
    // implement the wrapping half -- it refuses an actual tar rather than
    // reading through it. So a tar-wrapped layer fails at the call below
    // instead of being planned with a wrong base, and the plain case (a local
    // commit layer, which is what the publish path produces) needs no
    // adjustment. Inherited deviation, documented on `NewTarFileAdaptor`.
    const uint64_t tar_base = 0;
    std::shared_ptr<VirtualFile> unwrapped = local;
    const core::Expected<bool, std::string> is_tar = IsTarFile(local.get());
    if (!is_tar.ok()) return core::make_unexpected(is_tar.error());
    if (is_tar.value()) {
        const core::Expected<std::shared_ptr<VirtualFile>, std::string> tar =
            NewTarFileAdaptor(local);
        if (!tar.ok()) return core::make_unexpected(tar.error());
        unwrapped = tar.value();
    }

    OpenedLayer out;
    const core::Expected<int, std::string> is_zf = zfile::IsZFile(unwrapped.get());
    if (!is_zf.ok()) return core::make_unexpected(is_zf.error());
    if (is_zf.value() == 1) {
        // Verification off: the planner only reads the jump table, never a
        // payload, so a per-block checksum has nothing to check here.
        const core::Expected<std::shared_ptr<zfile::ZFileRO>, std::string> zf =
            zfile::ZFileOpenRo(unwrapped, false);
        if (!zf.ok()) return core::make_unexpected(zf.error());
        out.layout.kind = LayerLayout::kZFile;
        out.layout.zfile = zf.value();
        out.layout.tar_base = tar_base;
        // The decompressing view: index lookups address logical offsets.
        out.lsmt_file = zf.value();
    } else {
        out.layout.kind = LayerLayout::kRaw;
        out.layout.tar_base = tar_base;
        out.lsmt_file = unwrapped;
    }

    std::vector<std::shared_ptr<VirtualFile> > one;
    one.push_back(out.lsmt_file);
    const core::Expected<std::vector<ReadOnlyLayerMetadata>, std::string> meta =
        LoadReadonlyLayersMetadata(one);
    if (!meta.ok()) return core::make_unexpected(meta.error());
    if (meta.value().empty()) {
        return core::make_unexpected(std::string("layer metadata missing"));
    }
    out.metadata = meta.value()[0];
    return out;
}

/// Rust `push_aligned_blocks` — the object blocks an extent touches.
core::Expected<core::Unit, std::string> PushAlignedBlocks(uint64_t offset, uint64_t len,
                                                             uint64_t object_size,
                                                             std::vector<uint32_t>* out) {
    if (len == 0) return core::make_unexpected(std::string("empty extent to align"));

    const uint64_t object_blocks = DivCeil(object_size, kObjectBlockBytes);
    const uint64_t first = offset / kObjectBlockBytes;
    uint64_t last = (offset + len - 1) / kObjectBlockBytes;

    // Clamped to the object. An extent may run past the declared size when a
    // zfile's final compressed block is padded, and a block id beyond the
    // object cannot be fetched -- there is nothing there to read.
    const uint64_t limit = object_blocks == 0 ? 0 : object_blocks - 1;
    if (last > limit) last = limit;

    for (uint64_t block = first; block <= last; ++block) {
        if (block > 0xFFFFFFFFull) {
            return core::make_unexpected(std::string("block id overflow"));
        }
        out->push_back(static_cast<uint32_t>(block));
    }
    return core::Unit();
}

/// Rust `map_extent_blocks` — one physical extent of the logical layer file
/// mapped onto aligned block ids of the final object.
core::Expected<core::Unit, std::string> MapExtentBlocks(const LayerLayout& layout,
                                                           uint64_t logical_offset,
                                                           uint64_t logical_len,
                                                           uint64_t object_size,
                                                           std::vector<uint32_t>* out) {
    if (logical_len == 0) return core::Unit();

    if (layout.kind == LayerLayout::kRaw) {
        return PushAlignedBlocks(logical_offset + layout.tar_base, logical_len, object_size,
                                 out);
    }

    // Compressed: the logical extent spans some number of zfile blocks, and
    // each of those lives at its own compressed offset. Walking them through
    // the jump table is the only way to learn where they actually are.
    const uint64_t zblock = layout.zfile->Options().block_size;
    if (zblock == 0) return core::make_unexpected(std::string("zfile block size is zero"));

    const uint64_t first = logical_offset / zblock;
    const uint64_t last = (logical_offset + logical_len - 1) / zblock;
    for (uint64_t idx = first; idx <= last; ++idx) {
        uint64_t c_off = 0;
        uint64_t c_len = 0;
        const core::Expected<core::Unit, std::string> extent =
            layout.zfile->CompressedBlockExtent(idx * zblock, &c_off, &c_len);
        if (!extent.ok()) return core::make_unexpected(extent.error());
        const core::Expected<core::Unit, std::string> pushed =
            PushAlignedBlocks(c_off + layout.tar_base, c_len, object_size, out);
        if (!pushed.ok()) return pushed;
    }
    return core::Unit();
}

/// Rust `BlockPlanner`.
class BlockPlanner {
 public:
    static core::Expected<std::shared_ptr<BlockPlanner>, std::string> New(
        const std::vector<FinalLayerSource>* layers, uint64_t budget_bytes) {
        std::shared_ptr<BlockPlanner> p(new BlockPlanner());
        p->layers_ = layers;
        p->budget_bytes_ = budget_bytes;
        // The header is spent before any page is considered: it exists in the
        // artifact whether or not a single block fits.
        p->spent_bytes_ = kPlanHeaderBytes;

        for (size_t i = 0; i < layers->size(); ++i) {
            if ((*layers)[i].size == 0) {
                return core::make_unexpected("final layer '" + (*layers)[i].digest +
                                             "' has zero size");
            }
            const core::Expected<OpenedLayer, std::string> opened = OpenLayer((*layers)[i]);
            if (!opened.ok()) return core::make_unexpected(opened.error());
            p->opened_.push_back(opened.value());
        }

        std::vector<std::shared_ptr<VirtualFile> > lsmt_files;
        for (size_t i = 0; i < p->opened_.size(); ++i) {
            lsmt_files.push_back(p->opened_[i].lsmt_file);
        }
        const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> merged =
            OpenFilesRo(lsmt_files);
        if (!merged.ok()) return core::make_unexpected(merged.error());
        p->merged_ = merged.value();
        p->index_ = merged.value()->Index();

        p->object_of_layer_.assign(layers->size(), -1);
        p->metadata_emitted_.assign(layers->size(), false);
        return p;
    }

    core::Expected<PackPlan, std::string> Plan(const std::vector<uint32_t>& pages,
                                                  uint64_t mem_virtual_size,
                                                  PageObserver* on_page) {
        stats_.pages_total = pages.size();
        bool truncated = false;

        for (size_t i = 0; i < pages.size(); ++i) {
            const uint32_t page = pages[i];
            if (truncated) {
                // Every page after the first overflow is counted, not just
                // the one that did not fit: the plan is a prefix, and a
                // consumer needs to know how much of the trace it lost.
                ++stats_.pages_truncated_by_budget;
                continue;
            }

            const uint64_t voffset = static_cast<uint64_t>(page) * kPlanPageBytes;
            if (voffset + kPlanPageBytes > mem_virtual_size) {
                std::ostringstream o;
                o << "page 0x" << std::hex << page << " out of memory image range 0x"
                  << mem_virtual_size;
                return core::make_unexpected(o.str());
            }

            std::vector<lsmt::SegmentMapping> mappings;
            index_->Lookup(lsmt::Segment(voffset / kPlanSectorBytes,
                                         static_cast<uint32_t>(kPlanPageBytes /
                                                               kPlanSectorBytes)),
                           &mappings);

            std::vector<LayerBlock> span_blocks;
            const core::Expected<bool, std::string> backed =
                ResolveSpanBlocks(mappings, &span_blocks);
            if (!backed.ok()) return core::make_unexpected(backed.error());
            if (!backed.value()) {
                ++stats_.pages_zero_hole;
                if (on_page != 0) on_page->OnPage(page, std::vector<PageObserver::Resolved>());
                continue;
            }

            std::vector<PageObserver::Resolved> resolved;
            const core::Expected<bool, std::string> fitted =
                StageSpan(span_blocks, &resolved);
            if (!fitted.ok()) return core::make_unexpected(fitted.error());
            if (fitted.value()) {
                ++stats_.pages_backed;
                if (on_page != 0) on_page->OnPage(page, resolved);
            } else {
                truncated = true;
                ++stats_.pages_truncated_by_budget;
            }
        }
        return Finish();
    }

    core::Expected<PackPlan, std::string> PlanRangesImpl(
        const std::vector<std::pair<uint64_t, uint64_t> >& ranges, uint64_t mem_virtual_size) {
        bool truncated = false;

        for (size_t i = 0; i < ranges.size(); ++i) {
            const uint64_t voffset = ranges[i].first;
            const uint64_t vlen = ranges[i].second;
            if (vlen == 0 || voffset % kPlanPageBytes != 0 || vlen % kPlanPageBytes != 0 ||
                voffset + vlen > mem_virtual_size) {
                std::ostringstream o;
                o << "range 0x" << std::hex << voffset << "+0x" << vlen
                  << " out of memory image range 0x" << mem_virtual_size;
                return core::make_unexpected(o.str());
            }

            const size_t span_pages = static_cast<size_t>(vlen / kPlanPageBytes);
            // Counted in pages even though the lookup is per span, so the
            // stats mean the same thing on both paths.
            stats_.pages_total += span_pages;
            if (truncated) {
                stats_.pages_truncated_by_budget += span_pages;
                continue;
            }

            std::vector<lsmt::SegmentMapping> mappings;
            index_->Lookup(lsmt::Segment(voffset / kPlanSectorBytes,
                                         static_cast<uint32_t>(vlen / kPlanSectorBytes)),
                           &mappings);

            std::vector<LayerBlock> span_blocks;
            const core::Expected<bool, std::string> backed =
                ResolveSpanBlocks(mappings, &span_blocks);
            if (!backed.ok()) return core::make_unexpected(backed.error());
            if (!backed.value()) {
                stats_.pages_zero_hole += span_pages;
                continue;
            }

            std::vector<PageObserver::Resolved> resolved;
            const core::Expected<bool, std::string> fitted =
                StageSpan(span_blocks, &resolved);
            if (!fitted.ok()) return core::make_unexpected(fitted.error());
            if (fitted.value()) {
                stats_.pages_backed += span_pages;
            } else {
                truncated = true;
                stats_.pages_truncated_by_budget += span_pages;
            }
        }
        return Finish();
    }

 private:
    struct LayerBlock {
        size_t layer_idx;
        uint32_t block_id;
    };

    struct StagedBlock {
        uint16_t object;
        uint32_t block_id;
        bool metadata;
    };

    BlockPlanner() : layers_(0), budget_bytes_(0), spent_bytes_(0) {}
    BlockPlanner(const BlockPlanner&);
    BlockPlanner& operator=(const BlockPlanner&);

    /// Rust `stage_block` — register the layer's object lazily, then stage the
    /// block when it is new.
    ///
    /// Checked against both the committed selection and the pending stage: a
    /// span can name the same block twice (two mappings landing in one
    /// 256 KiB block), and staging it twice would pay for it twice.
    bool StageBlock(size_t layer_idx, uint32_t block_id,
                    const std::vector<StagedBlock>& staged, StagedBlock* out) {
        uint16_t object_idx = 0;
        if (object_of_layer_[layer_idx] >= 0) {
            object_idx = static_cast<uint16_t>(object_of_layer_[layer_idx]);
        } else {
            // Lazily: a layer that no page touches must not appear in the
            // object table, because every entry there costs budget.
            object_idx = static_cast<uint16_t>(objects_.size());
            PlannedObject object;
            object.digest = (*layers_)[layer_idx].digest;
            object.size = (*layers_)[layer_idx].size;
            object.layer = layer_idx;
            objects_.push_back(object);
            object_of_layer_[layer_idx] = static_cast<int32_t>(object_idx);
        }

        if (seen_.find(Key(object_idx, block_id)) != seen_.end()) return false;
        for (size_t i = 0; i < staged.size(); ++i) {
            if (staged[i].object == object_idx && staged[i].block_id == block_id) return false;
        }

        out->object = object_idx;
        out->block_id = block_id;
        out->metadata = false;
        return true;
    }

    /// Rust `resolve_span_blocks` — one lookup's mappings into `(layer,
    /// block)` dependencies.
    core::Expected<bool, std::string> ResolveSpanBlocks(
        const std::vector<lsmt::SegmentMapping>& mappings, std::vector<LayerBlock>* out) {
        bool backed = false;
        for (size_t i = 0; i < mappings.size(); ++i) {
            const lsmt::SegmentMapping& mapping = mappings[i];
            if (!mapping.HasPhysicalRange()) continue;
            backed = true;

            // The tag counts from the newest layer down, while `layers` is
            // bottom-to-top, so the two are mirror images of each other.
            // Getting this backwards would plan blocks of the wrong layer and
            // still look plausible.
            if (static_cast<size_t>(mapping.tag) >= layers_->size()) {
                std::ostringstream o;
                o << "mapping tag " << static_cast<int>(mapping.tag)
                  << " exceeds layer count " << layers_->size();
                return core::make_unexpected(o.str());
            }
            const size_t layer_idx = layers_->size() - 1 - static_cast<size_t>(mapping.tag);

            std::vector<uint32_t> raw_blocks;
            const core::Expected<core::Unit, std::string> mapped = MapExtentBlocks(
                opened_[layer_idx].layout, mapping.moffset * kPlanSectorBytes,
                static_cast<uint64_t>(mapping.Length()) * kPlanSectorBytes,
                (*layers_)[layer_idx].size, &raw_blocks);
            if (!mapped.ok()) return core::make_unexpected(mapped.error());

            for (size_t b = 0; b < raw_blocks.size(); ++b) {
                LayerBlock lb;
                lb.layer_idx = layer_idx;
                lb.block_id = raw_blocks[b];
                out->push_back(lb);
            }
        }
        return backed;
    }

    /// Rust `stage_span` — stage a resolved span as *one* budget unit.
    ///
    /// All-or-nothing on purpose: a page whose blocks half-fit would be
    /// unreadable, so the plan would have spent budget on something no
    /// consumer can use. On overflow the staging is rolled back and the
    /// caller stops here.
    core::Expected<bool, std::string> StageSpan(const std::vector<LayerBlock>& span_blocks,
                                                   std::vector<PageObserver::Resolved>* out) {
        const size_t stage_start_objects = objects_.size();
        const size_t stage_data_blocks = stats_.data_blocks;
        const size_t stage_meta_blocks = stats_.metadata_blocks;
        std::vector<StagedBlock> staged;

        // Metadata of every freshly-touched layer goes first: without the
        // header and index a consumer cannot interpret that layer's data
        // blocks at all, so they belong in the same budget unit, ahead of
        // the data.
        std::vector<size_t> staged_layers;
        for (size_t i = 0; i < span_blocks.size(); ++i) {
            const size_t layer_idx = span_blocks[i].layer_idx;
            if (std::find(staged_layers.begin(), staged_layers.end(), layer_idx) !=
                staged_layers.end()) {
                continue;
            }
            staged_layers.push_back(layer_idx);
            if (!metadata_emitted_[layer_idx]) {
                const core::Expected<core::Unit, std::string> meta =
                    StageMetadata(layer_idx, &staged);
                if (!meta.ok()) return core::make_unexpected(meta.error());
            }
        }

        for (size_t i = 0; i < span_blocks.size(); ++i) {
            StagedBlock block;
            if (StageBlock(span_blocks[i].layer_idx, span_blocks[i].block_id, staged, &block)) {
                staged.push_back(block);
                ++stats_.data_blocks;
            }
        }

        // The artifact's growth, not the payload's: a new object costs a
        // table entry, each block costs its payload plus an index entry, and
        // the payload acquires a 4-byte checksum per verify frame.
        uint64_t new_bytes = 0;
        for (size_t i = stage_start_objects; i < objects_.size(); ++i) {
            new_bytes += kPlanObjectEntryFixedBytes + objects_[i].digest.size();
        }
        new_bytes += static_cast<uint64_t>(staged.size()) *
                     (kObjectBlockBytes + kPlanEntryBytes);
        new_bytes += 4 * DivCeil(static_cast<uint64_t>(staged.size()) * kObjectBlockBytes,
                                 kPlanFrameBytes);

        if (spent_bytes_ + new_bytes > budget_bytes_) {
            objects_.resize(stage_start_objects);
            for (size_t i = 0; i < object_of_layer_.size(); ++i) {
                if (object_of_layer_[i] >= 0 &&
                    static_cast<size_t>(object_of_layer_[i]) >= stage_start_objects) {
                    object_of_layer_[i] = -1;
                }
            }
            stats_.data_blocks = stage_data_blocks;
            stats_.metadata_blocks = stage_meta_blocks;
            // `metadata_emitted_` is deliberately *not* rolled back, matching
            // Rust. It only matters for a later span, and there is none: the
            // caller stops at the first overflow.
            return false;
        }

        spent_bytes_ += new_bytes;
        for (size_t i = 0; i < staged.size(); ++i) {
            const size_t position = blocks_.size();
            seen_[Key(staged[i].object, staged[i].block_id)] = position;
            PlannedBlock block;
            block.object = staged[i].object;
            block.block_id = staged[i].block_id;
            block.metadata = staged[i].metadata;
            blocks_.push_back(block);
        }

        // The span's *full* resolution, including blocks first selected by an
        // earlier page: a consumer waiting on this page needs every block it
        // depends on, not only the ones this page happened to introduce.
        for (size_t i = 0; i < span_blocks.size(); ++i) {
            const int32_t object_idx = object_of_layer_[span_blocks[i].layer_idx];
            if (object_idx < 0) continue;
            const std::map<uint64_t, size_t>::const_iterator found =
                seen_.find(Key(static_cast<uint16_t>(object_idx), span_blocks[i].block_id));
            if (found == seen_.end()) continue;
            PageObserver::Resolved r;
            r.object = static_cast<uint16_t>(object_idx);
            r.block_id = span_blocks[i].block_id;
            r.position = found->second;
            out->push_back(r);
        }
        return true;
    }

    /// Rust `stage_metadata` — a layer's header and index region, staged the
    /// first time one of its data blocks is needed.
    core::Expected<core::Unit, std::string> StageMetadata(size_t layer_idx,
                                                             std::vector<StagedBlock>* staged) {
        if (metadata_emitted_[layer_idx]) return core::Unit();
        metadata_emitted_[layer_idx] = true;

        const OpenedLayer& layer = opened_[layer_idx];
        const uint64_t object_size = (*layers_)[layer_idx].size;

        std::vector<uint32_t> meta_raw;
        // The first page covers the header.
        const core::Expected<core::Unit, std::string> head =
            MapExtentBlocks(layer.layout, 0, kPlanPageBytes, object_size, &meta_raw);
        if (!head.ok()) return head;

        // Everything from the index offset to EOF: the index array and the
        // trailer. Taken as one extent because they are adjacent and a
        // consumer needs both to open the layer.
        const uint64_t index_offset = layer.metadata.index_offset;
        const uint64_t tail_len = layer.metadata.file_size > index_offset
                                      ? layer.metadata.file_size - index_offset
                                      : 0;
        if (tail_len > 0) {
            const core::Expected<core::Unit, std::string> tail = MapExtentBlocks(
                layer.layout, index_offset, tail_len, object_size, &meta_raw);
            if (!tail.ok()) return tail;
        }

        for (size_t i = 0; i < meta_raw.size(); ++i) {
            StagedBlock block;
            if (StageBlock(layer_idx, meta_raw[i], *staged, &block)) {
                block.metadata = true;
                staged->push_back(block);
                ++stats_.metadata_blocks;
            }
        }
        return core::Unit();
    }

    /// Rust `gap_stats` — missing-block runs in the complement of the
    /// selection, per contributing object.
    void GapStats() {
        for (size_t layer_idx = 0; layer_idx < layers_->size(); ++layer_idx) {
            const int32_t object_idx = object_of_layer_[layer_idx];
            if (object_idx < 0) continue;

            const uint64_t total = DivCeil((*layers_)[layer_idx].size, kObjectBlockBytes);
            std::vector<uint32_t> selected;
            for (std::map<uint64_t, size_t>::const_iterator it = seen_.begin();
                 it != seen_.end(); ++it) {
                if (ObjectOf(it->first) == static_cast<uint16_t>(object_idx)) {
                    selected.push_back(BlockOf(it->first));
                }
            }
            std::sort(selected.begin(), selected.end());

            uint64_t cursor = 0;
            for (size_t i = 0; i < selected.size(); ++i) {
                const uint64_t b = selected[i];
                if (b > cursor) {
                    ++stats_.gap_runs;
                    stats_.gap_blocks += static_cast<size_t>(b - cursor);
                }
                cursor = b + 1;
            }
            if (cursor < total) {
                ++stats_.gap_runs;
                stats_.gap_blocks += static_cast<size_t>(total - cursor);
            }
        }
    }

    core::Expected<PackPlan, std::string> Finish() {
        stats_.unique_blocks = seen_.size();
        stats_.payload_bytes = static_cast<uint64_t>(stats_.unique_blocks) * kObjectBlockBytes;
        GapStats();

        PackPlan plan;
        plan.objects = objects_;
        plan.blocks = blocks_;
        plan.stats = stats_;
        return plan;
    }

    /// `(object, block)` packed into one key, so the selection can live in a
    /// single ordered map.
    static uint64_t Key(uint16_t object, uint32_t block) {
        return (static_cast<uint64_t>(object) << 32) | static_cast<uint64_t>(block);
    }
    static uint16_t ObjectOf(uint64_t key) { return static_cast<uint16_t>(key >> 32); }
    static uint32_t BlockOf(uint64_t key) { return static_cast<uint32_t>(key & 0xFFFFFFFFull); }

    const std::vector<FinalLayerSource>* layers_;
    std::vector<OpenedLayer> opened_;
    /// Held so the index stays valid: it borrows the merged file's layers.
    std::shared_ptr<LsmtReadOnlyFile> merged_;
    std::shared_ptr<lsmt::LogIndex> index_;

    uint64_t budget_bytes_;
    uint64_t spent_bytes_;
    PlanStats stats_;

    std::vector<PlannedObject> objects_;
    /// Object index per layer, or -1 when the layer has not been touched.
    std::vector<int32_t> object_of_layer_;
    std::vector<PlannedBlock> blocks_;
    /// Position in `blocks_` of each selected `(object, block)`.
    std::map<uint64_t, size_t> seen_;
    std::vector<bool> metadata_emitted_;
};

core::Expected<core::Unit, std::string> ValidateLayers(
    const std::vector<FinalLayerSource>& layers) {
    if (layers.empty()) {
        return core::make_unexpected(std::string("no final layers to plan against"));
    }
    // The object index is a u16 in the artifact, so the table cannot name
    // more layers than that.
    if (layers.size() > 0x10000u) {
        return core::make_unexpected(std::string("too many layers"));
    }
    return core::Unit();
}

}  // namespace

core::Expected<PackPlan, std::string> PlanBlocksWithObserver(
    const std::vector<uint32_t>& pages, uint64_t mem_virtual_size,
    const std::vector<FinalLayerSource>& layers, uint64_t budget_bytes,
    PageObserver* on_page) {
    const core::Expected<core::Unit, std::string> valid = ValidateLayers(layers);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const core::Expected<std::shared_ptr<BlockPlanner>, std::string> planner =
        BlockPlanner::New(&layers, budget_bytes);
    if (!planner.ok()) return core::make_unexpected(planner.error());
    return planner.value()->Plan(pages, mem_virtual_size, on_page);
}

core::Expected<PackPlan, std::string> PlanBlocks(const std::vector<uint32_t>& pages,
                                                    uint64_t mem_virtual_size,
                                                    const std::vector<FinalLayerSource>& layers,
                                                    uint64_t budget_bytes) {
    return PlanBlocksWithObserver(pages, mem_virtual_size, layers, budget_bytes, 0);
}

core::Expected<PackPlan, std::string> PlanRanges(
    const std::vector<std::pair<uint64_t, uint64_t> >& ranges, uint64_t mem_virtual_size,
    const std::vector<FinalLayerSource>& layers, uint64_t budget_bytes) {
    const core::Expected<core::Unit, std::string> valid = ValidateLayers(layers);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    const core::Expected<std::shared_ptr<BlockPlanner>, std::string> planner =
        BlockPlanner::New(&layers, budget_bytes);
    if (!planner.ok()) return core::make_unexpected(planner.error());
    return planner.value()->PlanRangesImpl(ranges, mem_virtual_size);
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
