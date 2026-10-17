// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/{startup_pack,startup_manifest}.rs
#include "agentenv/storage/overlaybd/startup_pack.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace startup {

const char kTraceMagic[8] = {'A', 'E', 'N', 'V', 'T', 'R', 'C', '1'};
const char kManifestMagic[8] = {'A', 'E', 'N', 'V', 'M', 'F', '0', '1'};

namespace {

// Rust's `to_le_bytes` / `from_le_bytes`. Written byte-wise rather than by
// casting a struct: these formats cross a process boundary and are read by
// builds this code does not control, so the layout cannot depend on the
// host's endianness or on struct padding.
void PutU32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void PutU64(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

std::string Hex(uint64_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << v;
    return os.str();
}

/// Rust `validate_trace_offset` / `validate_page_offset` — the same two rules
/// in both files.
///
/// Alignment is required because the offset names a page, not an address: an
/// unaligned value would make the consumer read across a page boundary and
/// serve a guest half of one page and half of another.
///
/// The bound is checked on `offset + kPackPageBytes`, not on `offset`, because
/// the whole page is what gets read. An offset one page short of the end is in
/// range; the last byte of the image is not.
core::Expected<core::Unit, std::string> ValidatePageOffset(uint64_t mem_virtual_size,
                                                           uint64_t offset) {
    if (offset % kPackPageBytes != 0) {
        return core::make_unexpected("page offset " + Hex(offset) + " is not page-aligned");
    }
    // Rust `checked_add`. An offset within a page of UINT64_MAX would wrap and
    // then compare as in-range against any image size.
    if (offset > UINT64_MAX - kPackPageBytes) {
        return core::make_unexpected(std::string("page offset overflow"));
    }
    if (offset + kPackPageBytes > mem_virtual_size) {
        return core::make_unexpected("page offset " + Hex(offset) +
                                     " exceeds memory image size " + Hex(mem_virtual_size));
    }
    return core::Unit();
}

}  // namespace

// ---------------------------------------------------------------------------
// First-touch trace
// ---------------------------------------------------------------------------

core::Expected<std::vector<uint8_t>, std::string> EncodeTrace(
    uint64_t mem_virtual_size, const std::vector<uint64_t>& page_offsets) {
    if (page_offsets.empty()) {
        return core::make_unexpected(std::string("trace page list is empty"));
    }
    if (page_offsets.size() > static_cast<std::size_t>(kMaxTracePages)) {
        std::ostringstream why;
        why << "trace page count " << page_offsets.size() << " out of range";
        return core::make_unexpected(why.str());
    }

    std::vector<uint8_t> out;
    out.reserve(kTraceHeaderBytes + page_offsets.size() * 8);
    out.insert(out.end(), kTraceMagic, kTraceMagic + 8);
    PutU32(&out, kTraceFormatVersion);
    // The header size is written into the header. A consumer that knows a
    // later, larger header can still find the page list in this one.
    PutU32(&out, static_cast<uint32_t>(kTraceHeaderBytes));
    PutU64(&out, mem_virtual_size);
    PutU32(&out, static_cast<uint32_t>(page_offsets.size()));
    PutU32(&out, 0);  // reserved; decode requires these bytes to be zero

    for (std::size_t i = 0; i < page_offsets.size(); ++i) {
        // Validated on the way out, not only on the way in: a trace that
        // cannot be decoded is worse written than refused, because the failure
        // then surfaces on whichever node later tries to consume it.
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(mem_virtual_size, page_offsets[i]);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        PutU64(&out, page_offsets[i]);
    }
    return out;
}

core::Expected<std::pair<uint64_t, std::vector<uint64_t> >, std::string> DecodeTrace(
    const uint8_t* bytes, std::size_t size) {
    typedef std::pair<uint64_t, std::vector<uint64_t> > Decoded;

    if (bytes == NULL || size < kTraceHeaderBytes) {
        std::ostringstream why;
        why << "trace too short: " << size << " bytes";
        return core::make_unexpected(why.str());
    }
    if (std::memcmp(bytes, kTraceMagic, 8) != 0) {
        return core::make_unexpected(std::string("trace magic mismatch"));
    }
    const uint32_t version = GetU32(bytes + 8);
    if (version != kTraceFormatVersion) {
        std::ostringstream why;
        why << "unsupported trace version " << version;
        return core::make_unexpected(why.str());
    }
    const uint32_t header_bytes = GetU32(bytes + 12);
    if (static_cast<std::size_t>(header_bytes) != kTraceHeaderBytes) {
        std::ostringstream why;
        why << "unexpected trace header size " << header_bytes;
        return core::make_unexpected(why.str());
    }
    const uint64_t mem_virtual_size = GetU64(bytes + 16);
    const uint32_t page_count = GetU32(bytes + 24);
    // Rust checks the reserved bytes are zero. A producer that started using
    // them is writing a format this decoder does not understand, and the
    // version field alone would not have caught it.
    for (std::size_t i = 28; i < kTraceHeaderBytes; ++i) {
        if (bytes[i] != 0) {
            return core::make_unexpected(std::string("trace reserved header bytes are non-zero"));
        }
    }

    if (page_count == 0 || page_count > kMaxTracePages) {
        std::ostringstream why;
        why << "trace page count " << page_count << " out of range";
        return core::make_unexpected(why.str());
    }
    // Exact length, not a minimum: trailing bytes mean the writer and this
    // reader disagree about the format, and silently ignoring them would
    // accept a trace whose page list may also be misread.
    const std::size_t expected = kTraceHeaderBytes + static_cast<std::size_t>(page_count) * 8;
    if (size != expected) {
        std::ostringstream why;
        why << "trace length mismatch: expect " << expected << ", got " << size;
        return core::make_unexpected(why.str());
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(page_count);
    for (uint32_t i = 0; i < page_count; ++i) {
        const uint64_t offset = GetU64(bytes + kTraceHeaderBytes + static_cast<std::size_t>(i) * 8);
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(mem_virtual_size, offset);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        offsets.push_back(offset);
    }
    return Decoded(mem_virtual_size, offsets);
}

// ---------------------------------------------------------------------------
// Startup manifest
// ---------------------------------------------------------------------------

namespace {

/// Rust `validate_ranges`.
///
/// Sorts a copy by address and requires each range to start at or after the
/// previous one's end. Overlap has to be rejected rather than merged: two
/// ranges covering one page give the consumer two sources for the same guest
/// bytes, and nothing in the format says which wins.
core::Expected<core::Unit, std::string> ValidateRanges(
    uint64_t mem_virtual_size, const std::vector<std::pair<uint64_t, uint64_t> >& ranges) {
    std::vector<std::pair<uint64_t, uint64_t> > sorted = ranges;
    std::sort(sorted.begin(), sorted.end());

    uint64_t last_end = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const uint64_t start = sorted[i].first;
        const uint64_t len = sorted[i].second;
        if (start < last_end) {
            return core::make_unexpected(std::string("ranges overlap"));
        }
        if (len == 0 || len % kPackPageBytes != 0) {
            return core::make_unexpected("range length " + Hex(len) + " is not page-aligned");
        }
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(mem_virtual_size, start);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        if (start > UINT64_MAX - len) {
            return core::make_unexpected(std::string("range overflow"));
        }
        const uint64_t end = start + len;
        if (end > mem_virtual_size) {
            return core::make_unexpected("range " + Hex(start) + "+" + Hex(len) +
                                         " exceeds memory image size");
        }
        last_end = end;
    }
    return core::Unit();
}

}  // namespace

core::Expected<StartupManifest, std::string> BuildManifest(
    uint64_t mem_virtual_size, const std::vector<uint64_t>& page_offsets) {
    if (page_offsets.empty()) {
        return core::make_unexpected(std::string("page list is empty"));
    }
    for (std::size_t i = 0; i < page_offsets.size(); ++i) {
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(mem_virtual_size, page_offsets[i]);
        if (!ok.ok()) return core::make_unexpected(ok.error());
    }

    // Rank is the position in the first-touch list. The input is already
    // deduplicated by the recorder, so a repeated offset here would merge
    // into its own neighbour and inflate a range past the pages it covers.
    std::vector<std::pair<uint64_t, uint64_t> > ranked;  // (offset, rank)
    ranked.reserve(page_offsets.size());
    for (std::size_t i = 0; i < page_offsets.size(); ++i) {
        ranked.push_back(std::make_pair(page_offsets[i], static_cast<uint64_t>(i)));
    }
    // Sorted by offset only, so adjacency can be found in one pass. Rust uses
    // `sort_unstable_by_key(offset)`; ties cannot occur on a deduplicated
    // list, so the two orders agree.
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<uint64_t, uint64_t>& a,
                 const std::pair<uint64_t, uint64_t>& b) { return a.first < b.first; });

    // (start, len, best_rank)
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t> > merged;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const uint64_t offset = ranked[i].first;
        const uint64_t rank = ranked[i].second;
        if (!merged.empty() &&
            merged.back().first.first + merged.back().first.second == offset) {
            merged.back().first.second += kPackPageBytes;
            // The merged range is needed as early as its earliest page, so the
            // minimum rank is what the fetch order must use. Taking the last
            // page's rank would push a range needed immediately to the back.
            if (rank < merged.back().second) merged.back().second = rank;
        } else {
            merged.push_back(std::make_pair(std::make_pair(offset, kPackPageBytes), rank));
        }
    }

    // Rust `sort_by_key(rank)` — a *stable* sort there, so ranges sharing a
    // rank keep their address order. Ranks are unique here (each comes from a
    // distinct position in the input), but stable_sort keeps the two
    // implementations identical rather than relying on that.
    std::stable_sort(merged.begin(), merged.end(),
                     [](const std::pair<std::pair<uint64_t, uint64_t>, uint64_t>& a,
                        const std::pair<std::pair<uint64_t, uint64_t>, uint64_t>& b) {
                         return a.second < b.second;
                     });

    StartupManifest out;
    out.mem_virtual_size = mem_virtual_size;
    out.ranges.reserve(merged.size());
    for (std::size_t i = 0; i < merged.size(); ++i) {
        out.ranges.push_back(merged[i].first);
    }
    return out;
}

core::Expected<std::vector<uint8_t>, std::string> EncodeManifest(
    const StartupManifest& manifest) {
    if (manifest.prefix_pages.size() > static_cast<std::size_t>(kMaxPrefixPages)) {
        return core::make_unexpected(std::string("too many prefix pages"));
    }
    if (manifest.ranges.size() > static_cast<std::size_t>(kMaxRanges)) {
        return core::make_unexpected(std::string("too many ranges"));
    }
    for (std::size_t i = 0; i < manifest.prefix_pages.size(); ++i) {
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(manifest.mem_virtual_size, manifest.prefix_pages[i]);
        if (!ok.ok()) return core::make_unexpected(ok.error());
    }
    const core::Expected<core::Unit, std::string> ranges_ok =
        ValidateRanges(manifest.mem_virtual_size, manifest.ranges);
    if (!ranges_ok.ok()) return core::make_unexpected(ranges_ok.error());

    std::vector<uint8_t> out;
    out.reserve(kManifestHeaderBytes + manifest.prefix_pages.size() * 8 +
                manifest.ranges.size() * 16);
    out.insert(out.end(), kManifestMagic, kManifestMagic + 8);
    PutU32(&out, kManifestFormatVersion);
    PutU32(&out, static_cast<uint32_t>(kManifestHeaderBytes));
    PutU64(&out, manifest.mem_virtual_size);
    PutU32(&out, static_cast<uint32_t>(manifest.prefix_pages.size()));
    PutU32(&out, static_cast<uint32_t>(manifest.ranges.size()));
    // No reserved field here, unlike the trace: both counts fill the header,
    // which is why decode checks reserved bytes for one format and not the
    // other.
    for (std::size_t i = 0; i < manifest.prefix_pages.size(); ++i) {
        PutU64(&out, manifest.prefix_pages[i]);
    }
    for (std::size_t i = 0; i < manifest.ranges.size(); ++i) {
        PutU64(&out, manifest.ranges[i].first);
        PutU64(&out, manifest.ranges[i].second);
    }
    return out;
}

core::Expected<StartupManifest, std::string> DecodeManifest(const uint8_t* bytes,
                                                            std::size_t size) {
    if (bytes == NULL || size < kManifestHeaderBytes) {
        std::ostringstream why;
        why << "manifest too short: " << size << " bytes";
        return core::make_unexpected(why.str());
    }
    if (std::memcmp(bytes, kManifestMagic, 8) != 0) {
        return core::make_unexpected(std::string("manifest magic mismatch"));
    }
    const uint32_t version = GetU32(bytes + 8);
    if (version != kManifestFormatVersion) {
        std::ostringstream why;
        why << "unsupported manifest version " << version;
        return core::make_unexpected(why.str());
    }
    const uint32_t header_bytes = GetU32(bytes + 12);
    if (static_cast<std::size_t>(header_bytes) != kManifestHeaderBytes) {
        std::ostringstream why;
        why << "unexpected manifest header size " << header_bytes;
        return core::make_unexpected(why.str());
    }
    const uint64_t mem_virtual_size = GetU64(bytes + 16);
    const uint32_t prefix_page_count = GetU32(bytes + 24);
    const uint32_t range_count = GetU32(bytes + 28);
    // Both counts are bounded before either is used to size an allocation.
    if (prefix_page_count > kMaxPrefixPages || range_count > kMaxRanges) {
        return core::make_unexpected(std::string("manifest counts out of range"));
    }
    // Rust's `checked_add` chain. With 2^20 as the cap on each count the
    // product cannot overflow a 64-bit size_t, but the check is kept: it is
    // what makes the bound above load-bearing rather than incidental.
    const std::size_t prefix_bytes = static_cast<std::size_t>(prefix_page_count) * 8;
    const std::size_t range_bytes = static_cast<std::size_t>(range_count) * 16;
    if (prefix_bytes > SIZE_MAX - kManifestHeaderBytes ||
        range_bytes > SIZE_MAX - kManifestHeaderBytes - prefix_bytes) {
        return core::make_unexpected(std::string("manifest length overflow"));
    }
    const std::size_t expected = kManifestHeaderBytes + prefix_bytes + range_bytes;
    if (size != expected) {
        std::ostringstream why;
        why << "manifest length mismatch: expect " << expected << ", got " << size;
        return core::make_unexpected(why.str());
    }

    StartupManifest out;
    out.mem_virtual_size = mem_virtual_size;

    std::size_t cursor = kManifestHeaderBytes;
    out.prefix_pages.reserve(prefix_page_count);
    for (uint32_t i = 0; i < prefix_page_count; ++i) {
        const uint64_t offset = GetU64(bytes + cursor);
        const core::Expected<core::Unit, std::string> ok =
            ValidatePageOffset(mem_virtual_size, offset);
        if (!ok.ok()) return core::make_unexpected(ok.error());
        out.prefix_pages.push_back(offset);
        cursor += 8;
    }
    out.ranges.reserve(range_count);
    for (uint32_t i = 0; i < range_count; ++i) {
        const uint64_t start = GetU64(bytes + cursor);
        const uint64_t len = GetU64(bytes + cursor + 8);
        out.ranges.push_back(std::make_pair(start, len));
        cursor += 16;
    }
    // Checked as a set, after the whole table is read: overlap is a property
    // of the ranges together, so it cannot be decided one entry at a time.
    const core::Expected<core::Unit, std::string> ranges_ok =
        ValidateRanges(mem_virtual_size, out.ranges);
    if (!ranges_ok.ok()) return core::make_unexpected(ranges_ok.error());

    return out;
}

}  // namespace startup
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
