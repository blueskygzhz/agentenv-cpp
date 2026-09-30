// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/overlaybd_snapshot.rs
#include "agentenv/sandbox/firecracker/dirty_memory.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace obd = storage::overlaybd::lsmt;

core::Expected<uint64_t, std::string> CheckedI64ToU64(int64_t value,
                                                      const std::string& field) {
    if (value < 0) {
        std::ostringstream message;
        message << field << " must be non-negative, got " << value;
        return core::make_unexpected(message.str());
    }
    return static_cast<uint64_t>(value);
}

namespace {

bool SortByDestination(const obd::SegmentMapping& a, const obd::SegmentMapping& b) {
    if (a.Offset() != b.Offset()) return a.Offset() < b.Offset();
    if (a.Length() != b.Length()) return a.Length() < b.Length();
    return a.moffset < b.moffset;
}

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

/// Rust `fs::canonicalize(..).unwrap_or_else(|_| PathBuf::from(..))` — an
/// unresolvable path keeps its literal form rather than dropping out of the
/// comparison entirely.
std::string CanonicalizeOrLiteral(const std::string& path) {
    char buffer[PATH_MAX];
    if (::realpath(path.c_str(), buffer) != NULL) return std::string(buffer);
    return path;
}

/// Component-wise prefix test, matching Rust's `Path::starts_with`.
///
/// A raw string prefix would be wrong here: `/run/agentenv-extra` is not
/// inside `/run/agentenv`, and treating it as such would misclassify a durable
/// layer as runtime-owned.
bool PathStartsWith(const std::string& path, const std::string& prefix) {
    std::vector<std::string> path_parts;
    std::vector<std::string> prefix_parts;
    for (int pass = 0; pass < 2; ++pass) {
        const std::string& source = pass == 0 ? path : prefix;
        std::vector<std::string>& sink = pass == 0 ? path_parts : prefix_parts;
        std::size_t begin = 0;
        while (begin < source.size()) {
            const std::size_t end = source.find('/', begin);
            const std::string component = end == std::string::npos
                                              ? source.substr(begin)
                                              : source.substr(begin, end - begin);
            if (!component.empty()) sink.push_back(component);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    if (prefix_parts.size() > path_parts.size()) return false;
    for (std::size_t i = 0; i < prefix_parts.size(); ++i) {
        if (path_parts[i] != prefix_parts[i]) return false;
    }
    return true;
}

}  // namespace

core::Expected<std::vector<obd::SegmentMapping>, std::string> DirtyRangesToSegmentMappings(
    const DirtyMemoryRanges& dirty_ranges, uint64_t* memory_size_out) {
    // Only 4096 is accepted: every alignment check below assumes it, and a
    // different page size would make them all silently wrong.
    if (static_cast<uint64_t>(dirty_ranges.page_size) != kFirecrackerDirtyPageSize) {
        std::ostringstream message;
        message << "dirty memory page_size must be " << kFirecrackerDirtyPageSize << ", got "
                << dirty_ranges.page_size;
        return core::make_unexpected(message.str());
    }

    const core::Expected<uint64_t, std::string> memory_size =
        CheckedI64ToU64(dirty_ranges.memory_size, "memory_size");
    if (!memory_size.ok()) return core::make_unexpected(memory_size.error());

    if (memory_size.value() % kFirecrackerDirtyPageSize != 0) {
        std::ostringstream message;
        message << "dirty memory_size " << memory_size.value() << " is not "
                << kFirecrackerDirtyPageSize << "-byte aligned";
        return core::make_unexpected(message.str());
    }

    std::vector<obd::SegmentMapping> mappings;
    for (std::size_t i = 0; i < dirty_ranges.ranges.size(); ++i) {
        const DirtyMemoryRange& range = dirty_ranges.ranges[i];

        const core::Expected<uint64_t, std::string> base =
            CheckedI64ToU64(range.base_host_virt_addr, "base_host_virt_addr");
        if (!base.ok()) return core::make_unexpected(base.error());
        const core::Expected<uint64_t, std::string> offset =
            CheckedI64ToU64(range.image_offset, "image_offset");
        if (!offset.ok()) return core::make_unexpected(offset.error());
        const core::Expected<uint64_t, std::string> length =
            CheckedI64ToU64(range.length, "length");
        if (!length.ok()) return core::make_unexpected(length.error());

        uint64_t base_host_virt_addr = base.value();
        uint64_t image_offset        = offset.value();
        const uint64_t range_length  = length.value();

        // A zero-length dirty range carries no pages and would produce no
        // mappings; treating it as valid would hide a bug in the reporter.
        if (range_length == 0) {
            return core::make_unexpected(std::string("dirty memory range length must be positive"));
        }
        if (base_host_virt_addr % kFirecrackerDirtyPageSize != 0) {
            std::ostringstream message;
            message << "dirty range base_host_virt_addr " << Hex(base_host_virt_addr)
                    << " is not " << kFirecrackerDirtyPageSize << "-byte aligned";
            return core::make_unexpected(message.str());
        }
        if (image_offset % kFirecrackerDirtyPageSize != 0) {
            std::ostringstream message;
            message << "dirty range image_offset " << image_offset << " is not "
                    << kFirecrackerDirtyPageSize << "-byte aligned";
            return core::make_unexpected(message.str());
        }
        if (range_length % kFirecrackerDirtyPageSize != 0) {
            std::ostringstream message;
            message << "dirty range length " << range_length << " is not "
                    << kFirecrackerDirtyPageSize << "-byte aligned";
            return core::make_unexpected(message.str());
        }
        // Overflow-safe: a range ending past the declared image would make the
        // snapshot describe memory that does not exist.
        if (range_length > UINT64_MAX - image_offset ||
            image_offset + range_length > memory_size.value()) {
            std::ostringstream message;
            const uint64_t end = range_length > UINT64_MAX - image_offset
                                     ? UINT64_MAX
                                     : image_offset + range_length;
            message << "dirty range [" << image_offset << ", " << end << ") exceeds memory_size "
                    << memory_size.value();
            return core::make_unexpected(message.str());
        }

        // Byte counts become sector counts here: everything below is in
        // overlaybd sectors, not bytes.
        uint64_t remaining_sectors = range_length / kOverlaybdAlignment;
        while (remaining_sectors > 0) {
            // One mapping cannot describe more than kMaxLength sectors, so a
            // long run is split while staying contiguous on both sides.
            const uint64_t sector_count =
                std::min(remaining_sectors, static_cast<uint64_t>(obd::Segment::kMaxLength));
            mappings.push_back(obd::SegmentMapping(image_offset / kOverlaybdAlignment,
                                                   static_cast<uint32_t>(sector_count),
                                                   base_host_virt_addr / kOverlaybdAlignment,
                                                   false, 0));
            const uint64_t bytes = sector_count * kOverlaybdAlignment;
            base_host_virt_addr += bytes;
            image_offset += bytes;
            remaining_sectors -= sector_count;
        }
    }

    std::sort(mappings.begin(), mappings.end(), SortByDestination);

    // Destination sectors only. Two ranges may legitimately share a host
    // virtual address (the same page mapped twice), but two mappings landing
    // on one image sector would make the snapshot's contents depend on layer
    // ordering.
    for (std::size_t i = 0; i + 1 < mappings.size(); ++i) {
        const obd::SegmentMapping& previous = mappings[i];
        const obd::SegmentMapping& next     = mappings[i + 1];
        if (static_cast<uint64_t>(previous.Length()) > UINT64_MAX - previous.Offset()) {
            return core::make_unexpected(std::string("dirty memory segment end overflow"));
        }
        const uint64_t previous_end = previous.Offset() + previous.Length();
        if (previous_end > next.Offset()) {
            std::ostringstream message;
            message << "overlapping dirty memory destination ranges detected at image sectors "
                    << previous.Offset() << " and " << next.Offset();
            return core::make_unexpected(message.str());
        }
    }

    if (memory_size_out != NULL) *memory_size_out = memory_size.value();
    return mappings;
}

void SplitRuntimeSuffix(const std::vector<std::string>& lower_files,
                        const std::vector<std::string>& runtime_owned_roots,
                        std::vector<std::string>* durable_prefix,
                        std::vector<std::string>* runtime_owned_suffix) {
    durable_prefix->clear();
    runtime_owned_suffix->clear();

    std::size_t first_runtime_owned = lower_files.size();
    for (std::size_t i = 0; i < lower_files.size(); ++i) {
        // Canonicalize so a symlinked path is still recognised as runtime
        // owned; an unresolvable path falls back to its literal form, as Rust
        // does.
        const std::string path = CanonicalizeOrLiteral(lower_files[i]);

        bool owned = false;
        for (std::size_t r = 0; r < runtime_owned_roots.size(); ++r) {
            if (PathStartsWith(path, runtime_owned_roots[r])) owned = true;
        }
        if (owned) {
            first_runtime_owned = i;
            break;
        }
    }

    // Everything from the first runtime-owned layer on is runtime owned, even
    // a later layer that is not itself under such a root: the stack is
    // ordered, so a durable layer above a runtime layer still depends on it.
    for (std::size_t i = 0; i < lower_files.size(); ++i) {
        if (i < first_runtime_owned) {
            durable_prefix->push_back(lower_files[i]);
        } else {
            runtime_owned_suffix->push_back(lower_files[i]);
        }
    }
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
