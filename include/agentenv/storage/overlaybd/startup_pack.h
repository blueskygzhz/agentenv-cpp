// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/{startup_pack,startup_manifest}.rs
//
// The two wire formats of the startup memory domain. A *trace* is what the
// recorder produces on one node: the ordered, deduplicated list of guest pages
// a boot actually touched. A *manifest* is what the publisher turns that into:
// the same pages merged into address-contiguous ranges, with the range table
// ordered by when each range was first needed.
//
// Both are byte formats read by another process (the ublk daemon) and, for the
// manifest, by other nodes. They are versioned and fully validated on decode
// for that reason: a malformed or hostile index here does not fail a request,
// it decides which bytes get mapped into a guest's memory.
#ifndef AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_H_
#define AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace startup {

/// Rust `PACK_PAGE_BYTES` — guest page granularity of the first-touch list.
const uint64_t kPackPageBytes = 4096;

/// Rust `MAX_PACK_PAGES` — recorder page cap: 1 GiB of covered memory.
///
/// A cap on what the *recorder* keeps, not on what the formats below can
/// carry. Prefetching more than this stops paying for itself: the pages a boot
/// touches past the first gigabyte are increasingly the ones it would have
/// faulted in on its own anyway.
const uint32_t kMaxPackPages = 262144;

// ---------------------------------------------------------------------------
// First-touch trace  (Rust: startup_pack.rs)
// ---------------------------------------------------------------------------

/// Rust `TRACE_MAGIC` — `AENVTRC1`, 8 bytes, not NUL-terminated.
extern const char kTraceMagic[8];
/// Rust `TRACE_FORMAT_VERSION`.
const uint32_t kTraceFormatVersion = 1;
/// Rust `TRACE_HEADER_BYTES`.
const std::size_t kTraceHeaderBytes = 32;
/// Rust `MAX_TRACE_PAGES` — bounds parser allocation: 8 MiB of index bytes.
///
/// Checked *before* the page list is reserved, so a header claiming a
/// four-billion-page trace cannot make the decoder allocate on its word alone.
const uint32_t kMaxTracePages = 1u << 20;

/// Rust `encode_trace`.
///
/// `page_offsets` is the recorder's order and is preserved: position in this
/// list is the page's first-touch rank, which is the only record of what the
/// boot needed first. Sorting it here would destroy the information the
/// manifest is built from.
///
/// Rejects an empty list. A trace with no pages describes no prefetch, so
/// writing one would publish an artifact whose only possible effect is to make
/// consumers do work to learn there is nothing to do.
core::Expected<std::vector<uint8_t>, std::string> EncodeTrace(
    uint64_t mem_virtual_size, const std::vector<uint64_t>& page_offsets);

/// Rust `decode_trace` — returns the memory image size and the page offsets.
///
/// Every offset is re-validated against the decoded image size rather than
/// trusted: the length and count checks only prove the bytes are structurally
/// a trace, not that its offsets address memory this image has.
core::Expected<std::pair<uint64_t, std::vector<uint64_t> >, std::string> DecodeTrace(
    const uint8_t* bytes, std::size_t size);

// ---------------------------------------------------------------------------
// Startup manifest  (Rust: startup_manifest.rs)
// ---------------------------------------------------------------------------

/// Rust `MANIFEST_MAGIC` — `AENVMF01`, 8 bytes.
extern const char kManifestMagic[8];
/// Rust `MANIFEST_FORMAT_VERSION`.
///
/// Version 4, and refused on any mismatch. The manifest is consumed by the
/// ublk daemon, which may be a different build than the one that wrote it; a
/// decoder that guessed at an unknown version would map the wrong offsets into
/// guest memory and report success.
const uint32_t kManifestFormatVersion = 4;
/// Rust `MANIFEST_HEADER_BYTES`.
const std::size_t kManifestHeaderBytes = 32;
/// Rust `MAX_PREFIX_PAGES`.
const uint32_t kMaxPrefixPages = 1u << 20;
/// Rust `MAX_RANGES`.
const uint32_t kMaxRanges = 1u << 20;

/// Rust `struct StartupManifest`.
struct StartupManifest {
    uint64_t mem_virtual_size;
    /// Rust `prefix_pages` — pages delivered ahead of the range table.
    ///
    /// Empty for every current producer. Kept because the format carries the
    /// count: dropping the field would make this decoder reject manifests a
    /// future producer writes, and the two halves are already wired through
    /// the length check.
    std::vector<uint64_t> prefix_pages;
    /// Rust `ranges` — merged `(offset, len)` pairs, sorted by the earliest
    /// first-touch rank among the pages each one covers.
    ///
    /// The ordering is the point of the format. Addresses are already implied
    /// by the offsets, so sorting by address would carry no information; table
    /// position is instead the fetch order, which is what lets the consumer
    /// issue the earliest-needed range first.
    std::vector<std::pair<uint64_t, uint64_t> > ranges;

    StartupManifest() : mem_virtual_size(0) {}

    bool operator==(const StartupManifest& o) const {
        return mem_virtual_size == o.mem_virtual_size && prefix_pages == o.prefix_pages &&
               ranges == o.ranges;
    }
    bool operator!=(const StartupManifest& o) const { return !(*this == o); }
};

/// Rust `build_manifest`.
///
/// Merges address-adjacent pages into ranges, then orders the ranges by the
/// lowest first-touch rank they contain. Merging is what makes the prefetch
/// worth doing -- one request for a contiguous span instead of one per 4 KiB
/// page -- and the rank ordering is what keeps it useful after merging, since
/// a merged range is needed as early as its earliest page.
core::Expected<StartupManifest, std::string> BuildManifest(
    uint64_t mem_virtual_size, const std::vector<uint64_t>& page_offsets);

/// Rust `encode_manifest` — header, prefix pages, then the range table.
core::Expected<std::vector<uint8_t>, std::string> EncodeManifest(
    const StartupManifest& manifest);

/// Rust `decode_manifest`.
core::Expected<StartupManifest, std::string> DecodeManifest(const uint8_t* bytes,
                                                            std::size_t size);

}  // namespace startup
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_H_
