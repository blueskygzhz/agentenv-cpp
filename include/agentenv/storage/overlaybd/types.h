// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/  — LSMT-format read-only overlay block device.
//   HEADER (4KB) | segment[0] | segment[1] | ... | INDEX_TREE | TRAILER (4KB)
#ifndef AGENTENV_STORAGE_OVERLAYBD_TYPES_H_
#define AGENTENV_STORAGE_OVERLAYBD_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

// Magic in the header/trailer.
static const uint64_t kOverlaybdMagic = 0x4F42425Aull;  // 'OBBZ'

struct HeaderTrailer {
    uint64_t magic = kOverlaybdMagic;
    uint32_t version = 1;
    uint32_t flags = 0;
    uint64_t index_offset = 0;   // for trailer
    uint64_t index_size   = 0;
};

/// One entry in the index tree — maps a virtual (block-address) range to a
/// physical (file-offset) range within the overlaybd file.
struct SegmentMapping {
    uint64_t vaddr = 0;    // virtual disk offset (byte)
    uint64_t length = 0;   // bytes
    uint64_t poffset = 0;  // physical offset in file
    uint32_t flags = 0;    // compressed? / zeroed? / etc.
};

/// The complete parsed index of one overlaybd layer.
struct IndexTree {
    std::vector<SegmentMapping> segments;   // sorted by vaddr

    /// Binary-search a virtual address.
    core::Expected<const SegmentMapping*, core::AnyError>
        Lookup(uint64_t vaddr) const;
};

/// Read-only decoder over one overlaybd file.
class LayerReader {
 public:
    virtual ~LayerReader() = default;

    /// Open a file at `path` and load the trailer/index.
    static core::Expected<std::unique_ptr<LayerReader>, core::AnyError>
        Open(const std::string& path);

    virtual const IndexTree& Index() const = 0;

    /// Read `len` bytes starting at virtual `vaddr` into `dst`.
    virtual core::Expected<int64_t, core::AnyError>
        ReadAt(uint64_t vaddr, void* dst, int64_t len) = 0;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_TYPES_H_
