// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/backend.rs — `MemoryImageBackend`, and the
// `GuestRegionUffdMapping` / `PageState` types `handler.rs` defines and the
// backend consumes.
#ifndef AGENTENV_STORAGE_UFFD_CORE_BACKEND_H_
#define AGENTENV_STORAGE_UFFD_CORE_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace storage {

namespace overlaybd {
namespace lsmt {
class CompactSink;
}  // namespace lsmt
}  // namespace overlaybd

namespace uffd {

/// Rust `handler.rs :: PageState`.
enum PageState {
    /// Firecracker has touched the page. Treated as dirty on sight, because
    /// Firecracker's uffd does not implement UFFDIO_REGISTER_MODE_WP -- there
    /// is no write fault to wait for, so a read is all the evidence available.
    kPageAccessed,
    /// The guest released the page (virtio-balloon's MADV_DONTNEED/REMOVE).
    /// The next fault on it answers with a zero page, and a snapshot records
    /// it as a hole rather than dumping its stale contents.
    kPageRemoved
};

/// Rust `handler.rs :: GuestRegionUffdMapping` — one entry of the JSON array
/// Firecracker sends during the uffd handshake.
struct GuestRegionUffdMapping {
    /// Host virtual address where this region's guest memory lives.
    uint64_t base_host_virt_addr;
    /// Region size in bytes.
    uint64_t size;
    /// Byte offset of this region's data inside the snapshot memory image.
    ///
    /// NOT a guest physical address: MMIO holes between regions are collapsed
    /// so that image offsets stay contiguous. Using the GPA here would read
    /// from a hole for every region after the first.
    uint64_t offset;
    /// Page size configured for the region.
    uint64_t page_size;

    GuestRegionUffdMapping()
        : base_host_virt_addr(0), size(0), offset(0), page_size(0) {}

    /// `#[derive(Deserialize)]`.
    static core::Expected<std::vector<GuestRegionUffdMapping>, std::string> ParseList(
        const std::string& json);

    /// Rust's `Debug` impl, which prints the three addresses in hex. Kept
    /// because every use of these numbers is in hex and a decimal host
    /// virtual address is unreadable next to a fault address.
    std::string ToString() const;
};

/// Rust free function `hva_to_image_offset`, shared by the handler and the
/// overlaybd backend.
///
/// Both need it and neither owns the regions, so it stays a free function
/// rather than a method on one of them.
core::Expected<uint64_t, std::string> HvaToImageOffset(
    const std::vector<GuestRegionUffdMapping>& regions, uint64_t hva);

/// Rust `DashMap<u64, PageState>` — per-page state keyed by host virtual page
/// offset (`hva / page_size`).
///
/// A plain `std::map` behind the handler's own lock, not a sharded map: the
/// snapshot path iterates the whole thing and needs a consistent view, which
/// a lock-free sharded map cannot give without stopping writers anyway.
typedef std::map<uint64_t, PageState> PageStateMap;

/// Rust trait `MemoryImageBackend`.
///
/// The handler owns one of these and knows nothing about the image format, so
/// overlaybd and any future format share a single event loop.
class MemoryImageBackend {
 public:
    virtual ~MemoryImageBackend() {}

    /// Page size in bytes, typically 4096.
    virtual std::size_t PageSize() const = 0;

    /// Read one page at `offset` within the snapshot image.
    ///
    /// `offset` is page-aligned by the caller. An empty optional means the
    /// page is a hole and the handler fills zeros -- distinct from an error,
    /// which aborts the fault.
    virtual core::Expected<core::Optional<std::string>, std::string> ReadPageAtOffset(
        uint64_t offset) = 0;

    /// Rust `MemoryImageBackend::snapshot`, whose default implementation
    /// bails. Backends that cannot snapshot inherit this refusal.
    ///
    /// `peer_pid` is the Firecracker process, read with `process_vm_readv`.
    ///
    /// Rust takes an `Arc<dyn CompactWriter>` and wraps it in `CommitArgs`.
    /// Here the parameter is a `CompactSink`, because that is what this port's
    /// `CompactTo` accepts and handing it on is this parameter's only purpose.
    virtual core::Expected<std::string, std::string> Snapshot(
        int32_t /*peer_pid*/, const std::vector<GuestRegionUffdMapping>& /*mem_regions*/,
        const PageStateMap& /*dirty_pages*/, const std::string& /*layer_dir*/,
        overlaybd::lsmt::CompactSink* /*writer*/) {
        return core::make_unexpected(
            std::string("snapshot not implemented for this backend"));
    }
};

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_BACKEND_H_
