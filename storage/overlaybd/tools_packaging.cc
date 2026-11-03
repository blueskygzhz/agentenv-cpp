// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/tools/packaging.rs
#include "agentenv/storage/overlaybd/tools_packaging.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/local_file.h"

// `LayerStackCompactSource` — the adapter that binds `lsmt::CompactTo` to the
// io layer. Reused rather than re-implemented: a second source adapter is
// exactly where a short read would stop being treated as a failure.
#include "internal.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace tools {

namespace {

/// Rust `ALIGNMENT`.
const uint64_t kAlignment = lsmt::kLsmtAlignment;

}  // namespace

core::Expected<std::vector<lsmt::SegmentMapping>, std::string>
CreateMappingsFromSparse(VirtualFile* file, uint64_t start_offset) {
    if (file == NULL) return core::make_unexpected(std::string("null source file"));

    if (start_offset % kAlignment != 0) {
        std::ostringstream o;
        o << "start_offset " << start_offset
          << " of CreateMappingsFromSparse must be aligned to " << kAlignment << " bytes";
        return core::make_unexpected(o.str());
    }

    const core::Expected<uint64_t, std::string> size = file->Size();
    if (!size.ok()) return core::make_unexpected(size.error());
    const uint64_t file_size = size.value();

    std::vector<lsmt::SegmentMapping> mappings;
    // Rust returns an empty vector here rather than failing: a source with
    // nothing past the window is a layer with no data, not a bad request.
    if (file_size <= start_offset) return mappings;

    uint64_t cursor = start_offset;
    for (;;) {
        const core::Expected<core::Optional<uint64_t>, std::string> data =
            file->SeekData(cursor);
        if (!data.ok()) return core::make_unexpected(data.error());
        // No further data: everything from `cursor` on is a hole.
        if (!data.value().has_value()) break;
        const uint64_t begin = *data.value();
        if (begin >= file_size) break;

        const core::Expected<core::Optional<uint64_t>, std::string> hole =
            file->SeekHole(begin);
        if (!hole.ok()) return core::make_unexpected(hole.error());
        // Rust `v.min(file_size)` / `None => file_size`: an extent is never
        // allowed to run past the end, or the last mapping would claim blocks
        // the file does not have.
        uint64_t end = file_size;
        if (hole.value().has_value() && *hole.value() < file_size) end = *hole.value();
        if (end <= begin) break;

        // Rust `cursor = end.max(start_offset)`, kept verbatim as the defence
        // it is: `SeekData(cursor)` answers at or after `cursor`, and `cursor`
        // never precedes `start_offset`, so a correct seek implementation
        // cannot reach this. It only fires on a backend that answers before
        // the offset it was asked about -- where advancing past the extent is
        // the one response that cannot loop forever.
        if (begin < start_offset) {
            cursor = end > start_offset ? end : start_offset;
            continue;
        }

        if (begin % kAlignment != 0 || end % kAlignment != 0) {
            std::ostringstream o;
            o << "unaligned data extent [" << begin << ", " << end
              << ") reported for a file scanned by CreateMappingsFromSparse: extent "
                 "boundaries must be multiples of "
              << kAlignment << " bytes";
            return core::make_unexpected(o.str());
        }

        // One extent becomes as many mappings as its length needs: a segment's
        // length field is 14 bits, so a long extent cannot be described by a
        // single mapping.
        uint64_t logical = (begin - start_offset) / kAlignment;
        uint64_t physical = begin / kAlignment;
        uint64_t blocks = (end - begin) / kAlignment;
        while (blocks > 0) {
            const uint64_t limit = static_cast<uint64_t>(lsmt::Segment::kMaxLength);
            const uint64_t step = blocks < limit ? blocks : limit;
            // `zeroed = false`, `tag = 0`: these are real data blocks read out
            // of the single source layer.
            mappings.push_back(lsmt::SegmentMapping(logical, static_cast<uint32_t>(step),
                                                    physical, false, 0));
            logical += step;
            physical += step;
            blocks -= step;
        }
        cursor = end;
    }
    return mappings;
}

core::Expected<core::Unit, std::string>
PackageRawAsOverlaybd(const std::string& source, const std::string& output) {
    // Rust `output.parent().context(..)` then `create_dir_all`.
    const core::Optional<std::string> parent = core::fs::Parent(output);
    if (!parent.has_value() || parent->empty()) {
        return core::make_unexpected(
            std::string("output path should have a parent directory"));
    }
    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(*parent);
    if (!made.ok()) {
        return core::make_unexpected("create output directory failed: " + *parent + ": " +
                                     made.error());
    }

    // Rust `output.with_extension("commit.tmp")`. Appending instead keeps the
    // final name a strict prefix of the temporary one, which is what makes a
    // leftover `.tmp` attributable to its target; for the `.commit` names this
    // is called with, the two spellings coincide.
    const std::string tmp = output + ".tmp";

    const core::Expected<std::shared_ptr<LocalFile>, std::string> src =
        LocalFile::OpenRo(source);
    if (!src.ok()) {
        return core::make_unexpected("open source raw file failed: " + source + ": " +
                                     src.error());
    }

    // Rust takes the virtual size from the source's own length: the layer must
    // present the whole address space the raw file covered, holes included.
    const core::Expected<uint64_t, std::string> size = src.value()->Size();
    if (!size.ok()) {
        return core::make_unexpected("stat source raw file failed: " + source + ": " +
                                     size.error());
    }

    const core::Expected<std::vector<lsmt::SegmentMapping>, std::string> mappings =
        CreateMappingsFromSparse(src.value().get(), 0);
    if (!mappings.ok()) {
        return core::make_unexpected("scan sparse raw file failed: " + source + ": " +
                                     mappings.error());
    }

    // A single source layer, so every mapping's tag is 0. The vector is held
    // by reference by the adapter and must outlive the call, which it does.
    std::vector<std::shared_ptr<VirtualFile> > layers;
    layers.push_back(src.value());
    LayerStackCompactSource compact_source(layers);

    std::string err;
    if (!lsmt::CompactTo(tmp, &compact_source, mappings.value(), size.value(), &err)) {
        ::unlink(tmp.c_str());
        return core::make_unexpected("compact raw file as overlaybd failed: " + source +
                                     ": " + err);
    }

    if (::rename(tmp.c_str(), output.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        ::unlink(tmp.c_str());
        return core::make_unexpected("move sealed lower into place failed: " + output +
                                     ": " + why);
    }
    return core::Unit();
}

}  // namespace tools
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
