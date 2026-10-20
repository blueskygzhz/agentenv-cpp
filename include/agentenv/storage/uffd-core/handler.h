// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/handler.rs — `UffdHandle`.
//
// This is the page-fault handler for one restored Firecracker VM. Firecracker
// starts with its guest memory unpopulated and a userfaultfd registered over
// it; every first touch traps here, and this code answers with a page out of
// the snapshot image. Get it wrong and the guest does not crash -- it hangs on
// the faulting instruction, or runs on wrong memory.
//
// Three invariants carry the correctness of the whole file. Each one is a
// comment at its site too, but they are collected here because none of them is
// visible from the code shape:
//
//  1. REMOVE events are handled *synchronously, on the event loop*. The uffd
//     event stream is ordered: REMOVE followed by PAGEFAULT for one page means
//     the guest freed it and touched it again. Handling REMOVE off-thread lets
//     the fault handler write `Accessed` first and the late REMOVE overwrite it
//     with `Removed`, so the snapshot drops a page the guest is using.
//
//  2. The loop must NOT wait for in-flight fault handlers while a REMOVE is
//     pending. Each unread REMOVE holds the kernel's `mmap_changing` counter
//     above zero, and UFFDIO_COPY returns EAGAIN for as long as it is -- so
//     draining handlers before consuming the event deadlocks the VM.
//
//  3. EAGAIN on COPY/ZEROPAGE is a retry, EEXIST is success. EEXIST means
//     another thread already populated the page, which is the expected outcome
//     when two vCPUs fault on it at once; treating it as failure would abort a
//     fault that is in fact resolved.
//
// Rust runs the fault handlers as tokio tasks on a shared runtime. This port
// has no async runtime, so each fault is handled on a small thread pool and
// the loop thread only reads events. The division of labour is the same one
// and for the same reason: a fault that blocks on image I/O must not stop the
// loop from consuming the next REMOVE (invariant 2).
#ifndef AGENTENV_STORAGE_UFFD_CORE_HANDLER_H_
#define AGENTENV_STORAGE_UFFD_CORE_HANDLER_H_

#include <pthread.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/uffd-core/backend.h"
#include "agentenv/storage/uffd-core/uffd.h"

namespace agentenv {
namespace storage {

namespace overlaybd {
namespace lsmt {
class CompactSink;
}  // namespace lsmt
}  // namespace overlaybd

namespace uffd {

/// Rust `UffdHandle`.
///
/// Construction binds the Unix socket and returns; the handshake with
/// Firecracker and the fault loop both run in the background, because
/// Firecracker connects only after it has been told to resume and the caller
/// must be free to do that.
class UffdHandle {
 public:
    /// Rust `UffdHandle::new`.
    ///
    /// Takes ownership of `image`. Binds `uds_path` eagerly so that a path
    /// collision is reported to the caller rather than to a background thread
    /// that nobody is watching.
    static core::Expected<UffdHandle*, std::string> New(MemoryImageBackend* image,
                                                        const std::string& uds_path);

    ~UffdHandle();

    const std::string& uds_path() const { return uds_path_; }

    MemoryImageBackend* image() const { return image_.get(); }

    /// Rust `dirty_pages()`.
    ///
    /// Returns a copy, not a reference. Rust can hand out a `&DashMap` because
    /// the map is internally synchronised; here the map is guarded by this
    /// object's lock, and lending a reference would let a caller read it while
    /// the fault threads are writing.
    PageStateMap DirtyPages() const;

    /// Blocks until the handshake with Firecracker has either completed or
    /// failed, then reports which.
    ///
    /// Rust expresses this with `SetOnce` and makes every accessor wait on it.
    /// Exposed here because a test -- and `Snapshot` -- needs to know the
    /// mappings arrived, and because without it a failed handshake would be
    /// visible only in the log.
    core::Expected<core::Unit, std::string> WaitForHandshake(uint64_t timeout_ms);

    /// Rust `snapshot(layer_dir, writer)` — delegates to the backend with the
    /// peer pid, the regions and the tracked page state.
    core::Expected<std::string, std::string> Snapshot(const std::string& layer_dir,
                                                      overlaybd::lsmt::CompactSink* writer);

    /// Rust `stop()` — aborts the background tasks. Idempotent, and also run
    /// by the destructor, which is Rust's `Drop`.
    void Stop();

    /// Test seam: hand in an already-connected uffd and mappings instead of
    /// waiting for a Firecracker handshake.
    ///
    /// Takes ownership of `uffd`. Exists because the alternative for testing
    /// the fault loop is to run a real VMM, and then the invariants above are
    /// exercised only by accident.
    core::Expected<core::Unit, std::string> SyncForTesting(
        Uffd* uffd, const std::vector<GuestRegionUffdMapping>& regions, int32_t peer_pid);

 private:
    UffdHandle(MemoryImageBackend* image, const std::string& uds_path, int listener_fd);
    UffdHandle(const UffdHandle&);
    UffdHandle& operator=(const UffdHandle&);

    /// Rust `sync_with_fc` — accept, read the peer pid, receive the uffd and
    /// the mappings, and check every region's page size against the image.
    core::Expected<core::Unit, std::string> SyncWithFc();

    /// Rust `serve_pf` — the event loop.
    core::Expected<core::Unit, std::string> ServePf();

    /// Rust `handle_page_fault`.
    core::Expected<core::Unit, std::string> HandlePageFault(uint64_t fault_addr);

    /// Rust `record_remove_range`. Runs on the loop thread; see invariant 1.
    void RecordRemoveRange(uint64_t start, uint64_t end);

    /// Rust `zeropage` / `copy`, including their retry loops.
    core::Expected<core::Unit, std::string> ZeroPage(uint64_t hva);
    core::Expected<core::Unit, std::string> Copy(uint64_t hva, const std::string& data);

    static void* BackgroundMain(void* self);
    static void* FaultWorkerMain(void* self);

    /// Pops one queued fault address, waiting if the queue is empty. Returns
    /// false once the handle is stopping.
    bool NextFault(uint64_t* out);

    std::string uds_path_;
    std::unique_ptr<MemoryImageBackend> image_;
    int listener_fd_;

    /// Guards every field below it, plus `pages_`.
    mutable pthread_mutex_t lock_;
    /// Signals handshake completion and fault-queue arrivals.
    pthread_cond_t cond_;

    /// Rust `SetOnce<Result<UffdHandleInner>>`.
    bool handshake_done_;
    std::string handshake_error_;
    std::vector<GuestRegionUffdMapping> mem_regions_;
    std::unique_ptr<Uffd> uffd_;
    int32_t peer_pid_;

    PageStateMap pages_;

    bool stopping_;
    std::vector<uint64_t> fault_queue_;

    bool threads_started_;
    pthread_t bg_thread_;
    std::vector<pthread_t> workers_;
};

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_HANDLER_H_
