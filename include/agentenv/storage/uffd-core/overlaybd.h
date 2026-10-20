// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/overlaybd.rs — `OverlaybdMemoryImage`.
#ifndef AGENTENV_STORAGE_UFFD_CORE_OVERLAYBD_H_
#define AGENTENV_STORAGE_UFFD_CORE_OVERLAYBD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/uffd-core/backend.h"

namespace agentenv {
namespace storage {
namespace uffd {

/// Rust `OverlaybdMemoryImage` — a memory snapshot stored as an overlaybd
/// (LSMT) image.
///
/// Deliberately the same `ImageFile` type that disk images use, so that layer
/// opening, caching and the read path are one implementation for both memory
/// and disk rather than two that drift.
class OverlaybdMemoryImage : public MemoryImageBackend {
 public:
    /// Takes ownership of `file`.
    OverlaybdMemoryImage(overlaybd::ImageFile* file, uint64_t virtual_size,
                         std::size_t pagesize);

    uint64_t virtual_size() const { return virtual_size_; }
    overlaybd::ImageFile* file() const { return file_.get(); }

    std::size_t PageSize() const override { return pagesize_; }

    core::Expected<core::Optional<std::string>, std::string> ReadPageAtOffset(
        uint64_t offset) override;

    /// Rust `snapshot` — turn the handler's page state into an incremental
    /// LSMT layer, reading the live pages out of the VMM.
    core::Expected<std::string, std::string> Snapshot(
        int32_t peer_pid, const std::vector<GuestRegionUffdMapping>& mem_regions,
        const PageStateMap& dirty_pages, const std::string& layer_dir,
        overlaybd::lsmt::CompactSink* writer) override;

 private:
    OverlaybdMemoryImage(const OverlaybdMemoryImage&);
    OverlaybdMemoryImage& operator=(const OverlaybdMemoryImage&);

    std::unique_ptr<overlaybd::ImageFile> file_;
    uint64_t virtual_size_;
    std::size_t pagesize_;
};

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_OVERLAYBD_H_
