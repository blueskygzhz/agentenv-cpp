// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/io/{virtual_file,vfile_io,transient_io_ring}.rs
#ifndef AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_

#include <cstdint>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust trait `VirtualFile` — the read-side abstraction the LSMT stack layers on.
/// Both local files, remote blob readers, and the merged stack implement it.
class VirtualFile {
 public:
    virtual ~VirtualFile() {}
    /// Rust `pread`: read `len` bytes at byte `offset` into `dst`.
    virtual core::Expected<int64_t, core::AnyError>
        PRead(uint64_t offset, void* dst, int64_t len) = 0;
    /// Rust `size`.
    virtual int64_t Size() const = 0;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_
