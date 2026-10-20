// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/src/handler.rs — the `userfaultfd` crate calls made
// from `zeropage`, `copy` and `serve_pf`.
#include "agentenv/storage/uffd-core/uffd.h"

#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <sstream>

namespace agentenv {
namespace storage {
namespace uffd {
namespace {

/// `UFFDIO_API` handshake structs, needed only by `Create`.
struct UffdioApi {
    uint64_t api;
    uint64_t features;
    uint64_t ioctls;
};

static const uint64_t kUffdApi = 0xAA;
static const unsigned int kUffdioApi = _IOWR(0xAA, 0x3F, UffdioApi);

/// `UFFD_FEATURE_EVENT_REMOVE`. Without it the kernel never reports the
/// MADV_DONTNEED that virtio-balloon issues, and `record_remove_range` would
/// never run -- the snapshot would then dump freed pages as live data.
static const uint64_t kUffdFeatureEventRemove = 1ULL << 3;

std::string Errno(const char* what, int err) {
    std::ostringstream o;
    o << what << ": " << ::strerror(err) << " (errno " << err << ")";
    return o.str();
}

}  // namespace

Uffd::~Uffd() {
    if (fd_ >= 0) ::close(fd_);
}

int Uffd::Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

core::Expected<Uffd*, std::string> Uffd::Create(bool non_blocking) {
    // O_CLOEXEC always: this descriptor must not survive into a spawned VMM,
    // which would hold the fault handler open after this process is gone.
    int flags = 02000000;  // O_CLOEXEC
    if (non_blocking) flags |= 04000;  // O_NONBLOCK
#ifdef __NR_userfaultfd
    const int fd = static_cast<int>(::syscall(__NR_userfaultfd, flags));
#else
    const int fd = -1;
    errno = ENOSYS;
#endif
    if (fd < 0) return core::make_unexpected(Errno("userfaultfd", errno));

    UffdioApi api;
    api.api = kUffdApi;
    api.features = kUffdFeatureEventRemove;
    api.ioctls = 0;
    if (::ioctl(fd, kUffdioApi, &api) != 0) {
        const int err = errno;
        ::close(fd);
        return core::make_unexpected(Errno("UFFDIO_API", err));
    }
    return new Uffd(fd);
}

core::Expected<core::Unit, std::string> Uffd::Register(uint64_t start, uint64_t len) {
    UffdioRegister reg;
    reg.range.start = start;
    reg.range.len = len;
    // MISSING only. Firecracker does not implement WP mode, which is the
    // reason the handler treats every accessed page as dirty instead of
    // waiting for a write fault.
    reg.mode = kUffdioRegisterModeMissing;
    reg.ioctls = 0;
    if (::ioctl(fd_, kUffdioRegister, &reg) != 0) {
        return core::make_unexpected(Errno("UFFDIO_REGISTER", errno));
    }
    return core::Unit{};
}

core::Expected<core::Optional<UffdEvent>, std::string> Uffd::ReadEvent() {
    UffdMsg msg;
    ::memset(&msg, 0, sizeof(msg));
    const ssize_t n = ::read(fd_, &msg, sizeof(msg));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Nothing queued. Rust's `read_event` returns `Ok(None)` here and
            // the loop clears readiness and polls again.
            return core::Optional<UffdEvent>();
        }
        return core::make_unexpected(Errno("read uffd event", errno));
    }
    if (n == 0) {
        // EOF: the VMM closed its end. Reported as "no event" so the caller's
        // loop exits through its normal path rather than treating a shutdown
        // as a fault-handling failure.
        return core::Optional<UffdEvent>();
    }
    if (static_cast<std::size_t>(n) < sizeof(msg)) {
        std::ostringstream o;
        o << "short read from uffd: got " << n << " of " << sizeof(msg) << " byte(s)";
        return core::make_unexpected(o.str());
    }

    UffdEvent ev;
    ev.raw_event = msg.event;
    if (msg.event == kUffdEventPagefault) {
        ev.kind = UffdEvent::Pagefault;
        ev.addr = msg.arg.pagefault.address;
    } else if (msg.event == kUffdEventRemove) {
        ev.kind = UffdEvent::Remove;
        ev.start = msg.arg.remove.start;
        ev.end = msg.arg.remove.end;
    } else {
        ev.kind = UffdEvent::Unsupported;
    }
    return core::Optional<UffdEvent>(ev);
}

UffdIoResult Uffd::Copy(const void* src, uint64_t dst, std::size_t len, bool wake) {
    UffdioCopy arg;
    arg.dst = dst;
    arg.src = reinterpret_cast<uint64_t>(src);
    arg.len = static_cast<uint64_t>(len);
    arg.mode = wake ? 0 : kUffdioModeDontWake;
    arg.copy = 0;

    UffdIoResult out;
    if (::ioctl(fd_, kUffdioCopy, &arg) != 0) {
        out.error = errno;
        // The kernel also reports progress on failure: a negative `copy` is
        // the errno, a positive one is a partial transfer that the caller must
        // not mistake for "nothing happened".
        out.bytes = arg.copy > 0 ? arg.copy : 0;
        return out;
    }
    out.bytes = arg.copy;
    return out;
}

UffdIoResult Uffd::ZeroPage(uint64_t dst, std::size_t len, bool wake) {
    UffdioZeropage arg;
    arg.range.start = dst;
    arg.range.len = static_cast<uint64_t>(len);
    arg.mode = wake ? 0 : kUffdioModeDontWake;
    arg.zeropage = 0;

    UffdIoResult out;
    if (::ioctl(fd_, kUffdioZeropage, &arg) != 0) {
        out.error = errno;
        out.bytes = arg.zeropage > 0 ? arg.zeropage : 0;
        return out;
    }
    out.bytes = arg.zeropage;
    return out;
}

core::Expected<core::Unit, std::string> Uffd::Close() {
    if (fd_ < 0) return core::Unit{};
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) return core::make_unexpected(Errno("close uffd", errno));
    return core::Unit{};
}

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
