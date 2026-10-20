// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/process_vm_reader.rs — `ProcessVmReader` and
// `ScatterRegion`.
//
// Reads guest memory straight out of the live Firecracker process. This is
// what lets a memory snapshot be taken without asking Firecracker to write a
// memory file first: the dirty pages are pulled from the VMM's address space
// and compacted into an overlaybd layer in one pass.
//
// Note this is a *second* `ProcessVmReader` in the tree. The other one lives
// in `sandbox/firecracker/process_vm.h` and serves the dirty-range snapshot
// path there. They are not merged because they answer to different interfaces
// -- that one is a standalone reader, this one is an overlaybd `VirtualFile`
// and `CompactSource` feeding `CompactTo` -- and because Rust keeps them in
// separate crates for the same reason.
//
// Two details are load-bearing:
//
//  * `offset` is a **host virtual address**, not a file offset. Confusing the
//    two reads whatever happens to live at that file position.
//
//  * `process_vm_readv` may return a short read. A partial read treated as
//    complete leaves the rest of the buffer zeroed, which is indistinguishable
//    from legitimately zeroed guest memory -- the snapshot is then silently
//    wrong. A zero-byte return is not progress and must be an error, or the
//    loop spins forever.
#ifndef AGENTENV_STORAGE_UFFD_CORE_PROCESS_VM_READER_H_
#define AGENTENV_STORAGE_UFFD_CORE_PROCESS_VM_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace uffd {

/// Rust `ScatterRegion` — a remote address paired with a local destination.
struct ScatterRegion {
    /// Virtual address in the remote process.
    uint64_t va;
    uint8_t* buf;
    std::size_t len;

    ScatterRegion() : va(0), buf(0), len(0) {}
    ScatterRegion(uint64_t a, uint8_t* b, std::size_t n) : va(a), buf(b), len(n) {}
};

/// Rust `ProcessVmReader`.
///
/// Implements `VirtualFile` so it can be handed to the compaction path, and
/// `CompactSource` because that is the shape `CompactTo` takes in this port.
/// Only the read half is meaningful; the rest refuses, as Rust's `bail!` does.
class ProcessVmReader : public overlaybd::VirtualFile, public overlaybd::lsmt::CompactSource {
 public:
    explicit ProcessVmReader(int32_t pid) : pid_(pid) {}

    int32_t pid() const { return pid_; }

    /// Rust `read_scatter` — read several disjoint regions in one
    /// `process_vm_readv`.
    ///
    /// Returns the total bytes read, which the caller must compare against
    /// what it asked for.
    core::Expected<std::size_t, std::string> ReadScatter(std::vector<ScatterRegion>* regions);

    // -- VirtualFile ------------------------------------------------------

    /// Rust bails: this reader has no notion of a buffer it did not allocate.
    core::Expected<overlaybd::ByteBuffer, std::string> ReadAt(uint64_t offset,
                                                              size_t len) override;

    /// `offset` is a host virtual address. Fills `dst` completely or fails.
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                    size_t len) override;

    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;

    // -- CompactSource ----------------------------------------------------

    /// `src_byte_off` is a host virtual address, for the reason in the file
    /// header. `tag` is ignored: the snapshot path registers exactly one
    /// source layer, so every mapping carries tag 0.
    bool ReadAt(uint8_t tag, uint64_t src_byte_off, uint8_t* dst, uint64_t len) override;

 private:
    int32_t pid_;
};

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_PROCESS_VM_READER_H_
