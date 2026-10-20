// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/handler.rs
#include "agentenv/storage/uffd-core/handler.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/storage/uffd-core/scm.h"

namespace agentenv {
namespace storage {
namespace uffd {
namespace {

/// How many faults are served in parallel.
///
/// Rust spawns an unbounded `JoinSet` of tokio tasks; a fixed pool is the
/// closest thing without a runtime. The count matters less than the fact that
/// it is more than one: a single worker would serialise faults behind image
/// I/O, and the loop thread stays free either way (invariant 2).
static const std::size_t kFaultWorkers = 4;

/// Guard that locks on construction and unlocks on scope exit, so that an
/// early return cannot leave the handler's state locked.
class Lock {
 public:
    explicit Lock(pthread_mutex_t* m) : m_(m) { ::pthread_mutex_lock(m_); }
    ~Lock() { ::pthread_mutex_unlock(m_); }

 private:
    Lock(const Lock&);
    Lock& operator=(const Lock&);
    pthread_mutex_t* m_;
};

uint64_t NowMs() {
    struct timeval tv;
    ::gettimeofday(&tv, 0);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

std::string Hex(uint64_t v) {
    std::ostringstream o;
    o << "0x" << std::hex << v;
    return o.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UffdHandle::UffdHandle(MemoryImageBackend* image, const std::string& uds_path, int listener_fd)
    : uds_path_(uds_path),
      image_(image),
      listener_fd_(listener_fd),
      handshake_done_(false),
      peer_pid_(-1),
      stopping_(false),
      threads_started_(false) {
    ::pthread_mutex_init(&lock_, 0);
    ::pthread_cond_init(&cond_, 0);
}

core::Expected<UffdHandle*, std::string> UffdHandle::New(MemoryImageBackend* image,
                                                         const std::string& uds_path) {
    if (image == 0) {
        return core::make_unexpected(std::string("UffdHandle::New: no memory image backend"));
    }
    std::unique_ptr<MemoryImageBackend> owned(image);

    if (uds_path.size() + 1 > sizeof(reinterpret_cast<struct sockaddr_un*>(0)->sun_path)) {
        // Caught here because the kernel would otherwise silently truncate the
        // path and bind a socket at a name Firecracker will never connect to.
        std::ostringstream o;
        o << "uffd socket path is too long (" << uds_path.size() << " bytes)";
        return core::make_unexpected(o.str());
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::ostringstream o;
        o << "socket(AF_UNIX): " << ::strerror(errno);
        return core::make_unexpected(o.str());
    }

    struct sockaddr_un addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    ::memcpy(addr.sun_path, uds_path.c_str(), uds_path.size());

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd);
        std::ostringstream o;
        o << "bind uds in UffdHandle: " << ::strerror(err);
        return core::make_unexpected(o.str());
    }
    if (::listen(fd, 1) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(uds_path.c_str());
        std::ostringstream o;
        o << "listen uds in UffdHandle: " << ::strerror(err);
        return core::make_unexpected(o.str());
    }

    std::unique_ptr<UffdHandle> self(new UffdHandle(owned.release(), uds_path, fd));

    // Threads start last. Anything above this point can fail and unwind
    // without a running thread touching a half-built object.
    if (::pthread_create(&self->bg_thread_, 0, &UffdHandle::BackgroundMain, self.get()) != 0) {
        return core::make_unexpected(std::string("cannot start uffd handshake thread"));
    }
    for (std::size_t i = 0; i < kFaultWorkers; ++i) {
        pthread_t t;
        if (::pthread_create(&t, 0, &UffdHandle::FaultWorkerMain, self.get()) != 0) {
            // The handshake thread is already running, so the object cannot
            // simply be dropped: stop it and join before unwinding.
            self->threads_started_ = true;
            self->Stop();
            return core::make_unexpected(std::string("cannot start uffd fault worker"));
        }
        self->workers_.push_back(t);
    }
    self->threads_started_ = true;
    return self.release();
}

UffdHandle::~UffdHandle() {
    // Rust `impl Drop for UffdHandle`.
    Stop();
    if (listener_fd_ >= 0) ::close(listener_fd_);
    if (!uds_path_.empty()) ::unlink(uds_path_.c_str());
    ::pthread_cond_destroy(&cond_);
    ::pthread_mutex_destroy(&lock_);
}

void UffdHandle::Stop() {
    {
        Lock g(&lock_);
        if (stopping_) return;
        stopping_ = true;
        // Wakes the fault workers waiting on an empty queue and anyone in
        // WaitForHandshake.
        ::pthread_cond_broadcast(&cond_);
    }

    // Shut the listener down so an accept() that has not yet seen a peer
    // returns instead of holding the handshake thread forever.
    if (listener_fd_ >= 0) ::shutdown(listener_fd_, SHUT_RDWR);

    if (!threads_started_) return;
    ::pthread_join(bg_thread_, 0);
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        ::pthread_join(workers_[i], 0);
    }
    workers_.clear();
    threads_started_ = false;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

void* UffdHandle::BackgroundMain(void* arg) {
    UffdHandle* self = static_cast<UffdHandle*>(arg);
    const core::Expected<core::Unit, std::string> synced = self->SyncWithFc();
    {
        Lock g(&self->lock_);
        self->handshake_done_ = true;
        if (!synced.ok()) self->handshake_error_ = synced.error();
        ::pthread_cond_broadcast(&self->cond_);
    }
    if (!synced.ok()) {
        AGENTENV_ERROR("uffd handshake failed: " << synced.error());
        return 0;
    }
    AGENTENV_DEBUG("already synced with fc: " << self->uds_path_);

    const core::Expected<core::Unit, std::string> served = self->ServePf();
    if (served.ok()) {
        AGENTENV_INFO("uffd handler has finished event loop: " << self->uds_path_);
    } else {
        AGENTENV_ERROR("uffd handler event loop failed: " << served.error());
    }
    return 0;
}

core::Expected<core::Unit, std::string> UffdHandle::SyncWithFc() {
    int conn = -1;
    for (;;) {
        conn = ::accept(listener_fd_, 0, 0);
        if (conn >= 0) break;
        if (errno == EINTR) continue;
        if (errno == EINVAL) {
            // `Stop` shut the listener down while we were blocked here.
            return core::make_unexpected(std::string("uffd listener closed before handshake"));
        }
        std::ostringstream o;
        o << "accept on uffd uds: " << ::strerror(errno);
        return core::make_unexpected(o.str());
    }

    // Rust `stream.peer_cred()?.pid()`. The pid is what the snapshot path
    // later reads guest memory from, so a handshake without one is useless and
    // is refused now rather than at snapshot time.
    struct ucred cred;
    ::memset(&cred, 0, sizeof(cred));
    socklen_t cred_len = sizeof(cred);
    if (::getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
        const int err = errno;
        ::close(conn);
        std::ostringstream o;
        o << "get peer cred: " << ::strerror(err);
        return core::make_unexpected(o.str());
    }
    if (cred.pid <= 0) {
        ::close(conn);
        return core::make_unexpected(std::string("peer pid is None"));
    }

    // 1KB, as Rust: Firecracker sends one or two mappings.
    std::vector<char> buf(1024, 0);
    int received_fd = -1;
    std::size_t body_len = 0;
    // Retry for possible empty messages, as Firecracker's own handler does.
    for (int attempt = 0; attempt < 5; ++attempt) {
        const core::Expected<RecvWithFd, std::string> r =
            RecvWithFdFrom(conn, &buf[0], buf.size());
        if (!r.ok()) {
            ::close(conn);
            return core::make_unexpected(std::string("read uffd from uds failed: ") + r.error());
        }
        if (r.value().fd >= 0) {
            received_fd = r.value().fd;
            body_len = r.value().bytes;
            break;
        }
    }
    ::close(conn);

    if (received_fd < 0) {
        return core::make_unexpected(std::string("cannot get uffd from uds"));
    }
    std::unique_ptr<Uffd> uffd(Uffd::FromRawFd(received_fd));

    const std::string body(&buf[0], body_len);
    const core::Expected<std::vector<GuestRegionUffdMapping>, std::string> mappings =
        GuestRegionUffdMapping::ParseList(body);
    if (!mappings.ok()) {
        return core::make_unexpected(mappings.error() + ". Received body: " + body);
    }

    // Every region's page size must match the image's. A mismatch means the
    // handler would answer a fault with the wrong number of bytes, which
    // UFFDIO_COPY reports as a partial copy and the guest sees as corruption.
    const std::size_t img_page_size = image_->PageSize();
    for (std::size_t i = 0; i < mappings.value().size(); ++i) {
        if (mappings.value()[i].page_size != img_page_size) {
            std::ostringstream o;
            o << "region " << i << " page_size (" << Hex(mappings.value()[i].page_size)
              << ") does not match image page_size (" << Hex(img_page_size) << ")";
            return core::make_unexpected(o.str());
        }
    }

    Lock g(&lock_);
    mem_regions_ = mappings.value();
    uffd_.reset(uffd.release());
    peer_pid_ = static_cast<int32_t>(cred.pid);
    return core::Unit{};
}

core::Expected<core::Unit, std::string> UffdHandle::SyncForTesting(
    Uffd* uffd, const std::vector<GuestRegionUffdMapping>& regions, int32_t peer_pid) {
    std::unique_ptr<Uffd> owned(uffd);
    if (owned.get() == 0) {
        return core::make_unexpected(std::string("SyncForTesting: no uffd"));
    }
    Lock g(&lock_);
    if (handshake_done_) {
        return core::make_unexpected(std::string("UffdHandle inner has already been set"));
    }
    mem_regions_ = regions;
    uffd_.reset(owned.release());
    peer_pid_ = peer_pid;
    handshake_done_ = true;
    ::pthread_cond_broadcast(&cond_);
    return core::Unit{};
}

core::Expected<core::Unit, std::string> UffdHandle::WaitForHandshake(uint64_t timeout_ms) {
    const uint64_t deadline = NowMs() + timeout_ms;
    Lock g(&lock_);
    while (!handshake_done_ && !stopping_) {
        const uint64_t now = NowMs();
        if (now >= deadline) {
            return core::make_unexpected(
                std::string("UffdHandleInner has not been init"));
        }
        const uint64_t remaining = deadline - now;
        struct timespec ts;
        struct timeval tv;
        ::gettimeofday(&tv, 0);
        const uint64_t target_us =
            static_cast<uint64_t>(tv.tv_usec) + (remaining % 1000ULL) * 1000ULL;
        ts.tv_sec = static_cast<time_t>(tv.tv_sec + static_cast<long>(remaining / 1000ULL) +
                                        static_cast<long>(target_us / 1000000ULL));
        ts.tv_nsec = static_cast<long>((target_us % 1000000ULL) * 1000ULL);
        ::pthread_cond_timedwait(&cond_, &lock_, &ts);
    }
    if (!handshake_error_.empty()) {
        return core::make_unexpected(std::string("init UffdHandleInner failed: ") +
                                     handshake_error_);
    }
    if (!handshake_done_) {
        return core::make_unexpected(std::string("UffdHandleInner has not been init"));
    }
    return core::Unit{};
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> UffdHandle::ServePf() {
    int uffd_fd = -1;
    {
        Lock g(&lock_);
        if (uffd_.get() == 0) {
            return core::make_unexpected(std::string("UffdHandleInner has not been init"));
        }
        uffd_fd = uffd_->fd();
    }

    for (;;) {
        {
            Lock g(&lock_);
            if (stopping_) return core::Unit{};
        }

        struct pollfd pfd;
        pfd.fd = uffd_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        // A timeout rather than an indefinite wait, so `Stop` is observed even
        // when the guest never faults again.
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::ostringstream o;
            o << "wait for uffd events: " << ::strerror(errno);
            return core::make_unexpected(o.str());
        }
        if (ready == 0) continue;

        // Declared with a named initial value: `ev(core::Optional<UffdEvent>())`
        // would be parsed as a function declaration, not a variable.
        core::Optional<UffdEvent> no_event;
        core::Expected<core::Optional<UffdEvent>, std::string> ev = no_event;
        {
            Lock g(&lock_);
            if (stopping_ || uffd_.get() == 0) return core::Unit{};
            ev = uffd_->ReadEvent();
        }
        if (!ev.ok()) return core::make_unexpected(ev.error());
        if (!ev.value().has_value()) continue;

        const UffdEvent& event = *ev.value();
        if (event.kind == UffdEvent::Pagefault) {
            // Queued, not handled here: image I/O on this thread would stop
            // the loop from consuming the next REMOVE, and an unconsumed
            // REMOVE keeps `mmap_changing` above zero so every UFFDIO_COPY
            // fails with EAGAIN. That is invariant 2, and it deadlocks the VM.
            Lock g(&lock_);
            fault_queue_.push_back(event.addr);
            ::pthread_cond_broadcast(&cond_);
        } else if (event.kind == UffdEvent::Remove) {
            // Handled inline. The event stream is ordered, so a REMOVE that
            // arrives before a PAGEFAULT for the same page must be recorded
            // before that fault is served -- otherwise the fault writes
            // `Accessed` and a late REMOVE overwrites it, and the snapshot
            // drops a live page. That is invariant 1.
            //
            // Deliberately does not drain the fault workers first; see above.
            RecordRemoveRange(event.start, event.end);
        } else {
            std::ostringstream o;
            o << "UffdHandle recv unsupported event: " << static_cast<int>(event.raw_event);
            return core::make_unexpected(o.str());
        }
    }
}

void UffdHandle::RecordRemoveRange(uint64_t start, uint64_t end) {
    if (end <= start) {
        AGENTENV_WARN("ignore invalid uffd remove range: start=" << start << " end=" << end);
        return;
    }
    const uint64_t page_size = static_cast<uint64_t>(image_->PageSize());
    const uint64_t aligned_start = start & ~(page_size - 1);
    // Rust `end.next_multiple_of(page_size)`: a partially covered trailing
    // page is still gone, so it has to be marked.
    const uint64_t aligned_end = ((end + page_size - 1) / page_size) * page_size;

    Lock g(&lock_);
    for (uint64_t hva = aligned_start; hva < aligned_end; hva += page_size) {
        pages_[hva / page_size] = kPageRemoved;
    }
}

void* UffdHandle::FaultWorkerMain(void* arg) {
    UffdHandle* self = static_cast<UffdHandle*>(arg);
    uint64_t addr = 0;
    while (self->NextFault(&addr)) {
        const core::Expected<core::Unit, std::string> r = self->HandlePageFault(addr);
        if (!r.ok()) {
            AGENTENV_ERROR("Failed to handle page fault at " << Hex(addr) << ": " << r.error());
        }
    }
    return 0;
}

bool UffdHandle::NextFault(uint64_t* out) {
    Lock g(&lock_);
    for (;;) {
        if (!fault_queue_.empty()) {
            *out = fault_queue_.front();
            fault_queue_.erase(fault_queue_.begin());
            return true;
        }
        if (stopping_) return false;
        ::pthread_cond_wait(&cond_, &lock_);
    }
}

core::Expected<core::Unit, std::string> UffdHandle::HandlePageFault(uint64_t fault_addr) {
    const uint64_t page_size = static_cast<uint64_t>(image_->PageSize());
    fault_addr = fault_addr & ~(page_size - 1);
    const uint64_t host_virtual_pgoff = fault_addr / page_size;

    std::vector<GuestRegionUffdMapping> regions;
    bool was_removed = false;
    {
        Lock g(&lock_);
        if (uffd_.get() == 0) {
            return core::make_unexpected(std::string("UffdHandleInner has not been init"));
        }
        regions = mem_regions_;
        const PageStateMap::iterator it = pages_.find(host_virtual_pgoff);
        was_removed = (it != pages_.end() && it->second == kPageRemoved);
        // Rust's `insert` returns the previous value; the state is overwritten
        // either way, because the page is about to be populated.
        pages_[host_virtual_pgoff] = kPageAccessed;
    }

    // A page the guest had released reads back as zeros, not as its stale
    // contents: the guest is entitled to assume a freed-then-faulted page is
    // clean, and the snapshot records it as a hole to match.
    if (was_removed) return ZeroPage(fault_addr);

    const core::Expected<uint64_t, std::string> image_offset =
        HvaToImageOffset(regions, fault_addr);
    if (!image_offset.ok()) return core::make_unexpected(image_offset.error());

    const core::Expected<core::Optional<std::string>, std::string> data =
        image_->ReadPageAtOffset(image_offset.value());
    if (!data.ok()) {
        return core::make_unexpected(std::string("read page data at image_offset ") +
                                     Hex(image_offset.value()) + ": " + data.error());
    }
    if (!data.value().has_value()) return ZeroPage(fault_addr);
    return Copy(fault_addr, *data.value());
}

// ---------------------------------------------------------------------------
// UFFDIO_ZEROPAGE / UFFDIO_COPY, with their retry loops
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> UffdHandle::ZeroPage(uint64_t hva) {
    const std::size_t page_size = image_->PageSize();
    for (;;) {
        UffdIoResult r;
        {
            Lock g(&lock_);
            if (stopping_) return core::Unit{};
            if (uffd_.get() == 0) {
                return core::make_unexpected(std::string("UffdHandleInner has not been init"));
            }
            r = uffd_->ZeroPage(hva, page_size, true);
        }
        if (r.error == 0) {
            if (r.bytes == static_cast<int64_t>(page_size)) return core::Unit{};
            AGENTENV_WARN("uffd zeropage succeeded but with only partial data: size="
                          << r.bytes << " hva=" << Hex(hva));
        } else if (r.error == EAGAIN) {
            // `mmap_changing` is non-zero: a REMOVE/REMAP is in flight. The
            // loop thread will consume it, so retrying is what unblocks this.
        } else if (r.error == EEXIST) {
            // Another worker already populated the page. Resolved, not failed.
            return core::Unit{};
        } else {
            std::ostringstream o;
            o << "zeropage at " << Hex(hva) << " failed: " << ::strerror(r.error);
            return core::make_unexpected(o.str());
        }
        ::sched_yield();
    }
}

core::Expected<core::Unit, std::string> UffdHandle::Copy(uint64_t hva,
                                                         const std::string& data) {
    if (data.empty()) {
        return core::make_unexpected(std::string("uffd copy: empty page data"));
    }
    for (;;) {
        UffdIoResult r;
        {
            Lock g(&lock_);
            if (stopping_) return core::Unit{};
            if (uffd_.get() == 0) {
                return core::make_unexpected(std::string("UffdHandleInner has not been init"));
            }
            r = uffd_->Copy(data.data(), hva, data.size(), true);
        }
        if (r.error == 0) {
            if (r.bytes == static_cast<int64_t>(data.size())) return core::Unit{};
            // Rust warns and retries rather than returning: a partial copy
            // leaves the rest of the page unpopulated, and the faulting thread
            // is still blocked on it.
            AGENTENV_WARN("uffd succeeded but with only partial data: copy_size="
                          << r.bytes << " buf_len=" << data.size() << " hva=" << Hex(hva));
        } else if (r.error == EAGAIN) {
            // See ZeroPage.
        } else if (r.error == EEXIST) {
            return core::Unit{};
        } else {
            std::ostringstream o;
            o << "uffd copy failed: " << ::strerror(r.error);
            return core::make_unexpected(o.str());
        }
        ::sched_yield();
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

PageStateMap UffdHandle::DirtyPages() const {
    Lock g(&lock_);
    return pages_;
}

core::Expected<std::string, std::string> UffdHandle::Snapshot(
    const std::string& layer_dir, overlaybd::lsmt::CompactSink* writer) {
    std::vector<GuestRegionUffdMapping> regions;
    PageStateMap pages;
    int32_t pid = -1;
    {
        Lock g(&lock_);
        if (!handshake_done_ || uffd_.get() == 0) {
            return core::make_unexpected(std::string("UffdHandleInner has not been init"));
        }
        if (!handshake_error_.empty()) {
            return core::make_unexpected(std::string("init UffdHandleInner failed: ") +
                                         handshake_error_);
        }
        regions = mem_regions_;
        pages = pages_;
        pid = peer_pid_;
    }
    return image_->Snapshot(pid, regions, pages, layer_dir, writer);
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
