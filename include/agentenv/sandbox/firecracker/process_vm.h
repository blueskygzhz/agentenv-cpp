// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/process_vm_reader.rs — reads guest memory
// straight out of the live Firecracker process with `process_vm_readv`.
//
// This is what makes a direct memory snapshot possible: instead of asking
// Firecracker to write a memory file and then reading it back, the dirty pages
// are pulled from the VMM's address space and compacted into an overlaybd layer
// in one pass.
//
// Two details are load-bearing:
//
//  * The `offset` is a **host virtual address**, not a file offset. The caller
//    gets those from `DirtyMemoryRange::base_host_virt_addr`, and confusing
//    the two would read whatever happens to live at that file position.
//
//  * `process_vm_readv` is allowed to return a short read. The loop must keep
//    going until the destination is full, because a partial read that is
//    treated as complete leaves the rest of the buffer as zeros — which looks
//    like legitimately zeroed guest memory and corrupts the snapshot silently.
//    A zero-byte return is *not* progress and must be an error, or the loop
//    would spin forever.
#ifndef AGENTENV_SANDBOX_FIRECRACKER_PROCESS_VM_H_
#define AGENTENV_SANDBOX_FIRECRACKER_PROCESS_VM_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {
namespace firecracker {

/// Rust struct `ProcessVmReader`.
class ProcessVmReader {
 public:
    explicit ProcessVmReader(int64_t pid) : pid_(pid) {}

    int64_t pid() const { return pid_; }

    /// Rust `read_exact_remote` — fills `dst` completely or fails.
    ///
    /// `remote_addr` is a host virtual address in the target process.
    core::Expected<core::Unit, std::string> ReadExactRemote(uint64_t remote_addr, void* dst,
                                                            std::size_t len) const;

    /// Rust `VirtualFile::read_at` — returns exactly `len` bytes.
    core::Expected<std::string, std::string> ReadAt(uint64_t remote_addr,
                                                    std::size_t len) const;

 private:
    int64_t pid_;
};

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_FIRECRACKER_PROCESS_VM_H_
