// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/overlaybd.rs
#include "agentenv/storage/uffd-core/overlaybd.h"

#include <algorithm>
#include <sstream>

#include "agentenv/core/identity.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/uffd-core/process_vm_reader.h"

namespace agentenv {
namespace storage {
namespace uffd {
namespace {

/// Rust's local `const ALIGNMENT: u64 = 512`.
static const uint64_t kAlignment = 512;

/// Rust `impl Ord for SegmentMapping`, which compares `segment.offset` alone.
///
/// Matching it exactly matters even though every mapping built below has a
/// distinct offset: `CompactTo` consumes the order, and a comparator that
/// broke ties differently would reorder nothing here but would quietly differ
/// from the index that the rest of the format is written against.
struct ByOffset {
    bool operator()(const overlaybd::lsmt::SegmentMapping& a,
                    const overlaybd::lsmt::SegmentMapping& b) const {
        return a.segment.offset < b.segment.offset;
    }
};

}  // namespace

OverlaybdMemoryImage::OverlaybdMemoryImage(overlaybd::ImageFile* file, uint64_t virtual_size,
                                           std::size_t pagesize)
    : file_(file), virtual_size_(virtual_size), pagesize_(pagesize) {}

core::Expected<core::Optional<std::string>, std::string>
OverlaybdMemoryImage::ReadPageAtOffset(uint64_t offset) {
    if (file_.get() == 0) {
        return core::make_unexpected(std::string("overlaybd memory image has no file"));
    }
    const core::Expected<overlaybd::ByteBuffer, std::string> data =
        file_->ReadAt(offset, pagesize_);
    if (!data.ok()) {
        std::ostringstream o;
        o << "read_at offset = 0x" << std::hex << offset << " of overlaybd: " << data.error();
        return core::make_unexpected(o.str());
    }
    // Rust maps the read to `Some(..)` unconditionally: this backend has no
    // hole concept of its own, because the LSMT layer below already answers a
    // hole with zeros. Returning `nullopt` here would make the handler fill a
    // zero page too -- the same bytes by a longer route -- but it would also
    // hide a genuinely short read, which is why the length is checked instead.
    if (data.value().size() != pagesize_) {
        std::ostringstream o;
        o << "read_at offset = 0x" << std::hex << offset << " of overlaybd returned "
          << std::dec << data.value().size() << " of " << pagesize_ << " byte(s)";
        return core::make_unexpected(o.str());
    }
    return core::Optional<std::string>(
        std::string(reinterpret_cast<const char*>(&data.value()[0]), data.value().size()));
}

core::Expected<std::string, std::string> OverlaybdMemoryImage::Snapshot(
    int32_t peer_pid, const std::vector<GuestRegionUffdMapping>& mem_regions,
    const PageStateMap& dirty_pages, const std::string& /*layer_dir*/,
    overlaybd::lsmt::CompactSink* writer) {
    if (writer == 0) {
        return core::make_unexpected(std::string("overlaybd snapshot: no writer"));
    }
    if (file_.get() == 0) {
        return core::make_unexpected(std::string("overlaybd memory image has no file"));
    }
    if (pagesize_ == 0 || (pagesize_ % kAlignment) != 0) {
        // The mapping length below is expressed in 512B units, so a page size
        // that is not a multiple of the alignment would silently truncate.
        std::ostringstream o;
        o << "overlaybd snapshot: page size " << pagesize_ << " is not a multiple of "
          << kAlignment;
        return core::make_unexpected(o.str());
    }

    ProcessVmReader reader(peer_pid);

    std::vector<overlaybd::lsmt::SegmentMapping> mappings;
    mappings.reserve(dirty_pages.size());
    for (PageStateMap::const_iterator it = dirty_pages.begin(); it != dirty_pages.end(); ++it) {
        const uint64_t host_virtual_pgoff = it->first;
        const uint64_t host_virtual_start = host_virtual_pgoff * static_cast<uint64_t>(pagesize_);
        const core::Expected<uint64_t, std::string> image_offset =
            HvaToImageOffset(mem_regions, host_virtual_start);
        if (!image_offset.ok()) return core::make_unexpected(image_offset.error());

        const uint32_t length_sectors = static_cast<uint32_t>(pagesize_ / kAlignment);
        if (it->second == kPageAccessed) {
            // The physical offset is the **host virtual address** in 512B
            // units, not a position in any file. `CompactTo` will ask the
            // source for `moffset * 512`, and the source is a
            // `process_vm_readv` reader -- so this field is how the HVA gets
            // carried to it. Writing a real file offset here would compact
            // whatever lives at that offset in the parent layer.
            mappings.push_back(overlaybd::lsmt::SegmentMapping(image_offset.value() / kAlignment,
                                                         length_sectors,
                                                         host_virtual_start / kAlignment,
                                                         /*zeroed=*/false, /*tag=*/0));
        } else {
            // Released by the guest: recorded as a zero mapping that consumes
            // no space, so the layer does not carry the page's stale contents.
            mappings.push_back(overlaybd::lsmt::SegmentMapping(image_offset.value() / kAlignment,
                                                         length_sectors, /*moffset=*/0,
                                                         /*zeroed=*/true, /*tag=*/0));
        }
    }

    // Rust `mappings.sort_unstable()`.
    std::sort(mappings.begin(), mappings.end(), ByOffset());

    const core::Uuid uuid = core::Uuid::GenV4();
    const core::Expected<std::string, std::string> parent_uuid = file_->GetUuid(0);
    if (!parent_uuid.ok()) {
        return core::make_unexpected(std::string("get uuid of parent layer: ") +
                                     parent_uuid.error());
    }

    overlaybd::lsmt::CompactCommitArgs args;
    args.uuid = uuid.ToString();
    args.parent_uuid = parent_uuid.value();
    // Rust sets `concurrency = 64`; this port's `CompactTo` copies blocks
    // sequentially, which is the `concurrency = 1` case. The difference is
    // throughput only -- the output bytes and the index are identical.

    std::string err;
    if (!overlaybd::lsmt::CompactTo(writer, &reader, mappings, virtual_size_, args, &err)) {
        return core::make_unexpected(std::string("compact_to during overlaybd snapshot: ") +
                                     err);
    }
    return uuid.ToString();
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
