// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/ — the `userfaultfd` crate's `Uffd`, as used by
// `handler.rs`.
//
// The ABI is declared here rather than included from <linux/userfaultfd.h>,
// which is not present on every build host (it ships with linux-headers, and
// this repo builds without them). userfaultfd's uapi is stable kernel ABI, so
// declaring it is safe; what would not be safe is silently compiling the
// handler out, because a snapshot restore that finds no page-fault handler
// hangs the guest on its first access instead of failing.
//
// Two shapes here are load-bearing and easy to get wrong:
//
//  * `uffd_msg` is `__packed` in the kernel. Without the attribute the
//    compiler is free to insert padding after the 1-byte `event`, and every
//    fault address read out of the union would be garbage.
//
//  * `copy`/`zeropage` are `__s64` *out* parameters, not just status. The
//    kernel reports the number of bytes transferred there, and a short
//    transfer is reported as success at the ioctl level. Callers must look at
//    the count, which is why the wrapper returns it rather than a bare bool.
#ifndef AGENTENV_STORAGE_UFFD_CORE_UFFD_H_
#define AGENTENV_STORAGE_UFFD_CORE_UFFD_H_

#include <sys/ioctl.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace storage {
namespace uffd {

// ---------------------------------------------------------------------------
// Kernel uapi (include/uapi/linux/userfaultfd.h)
// ---------------------------------------------------------------------------

/// `UFFD_EVENT_*`. Only PAGEFAULT and REMOVE are handled; the rest are an
/// error, matching Rust's `event => return Err(... unsupported event ...)`.
static const uint8_t kUffdEventPagefault = 0x12;
static const uint8_t kUffdEventFork = 0x13;
static const uint8_t kUffdEventRemap = 0x14;
static const uint8_t kUffdEventRemove = 0x15;
static const uint8_t kUffdEventUnmap = 0x16;

/// `UFFDIO_REGISTER_MODE_*`.
static const uint64_t kUffdioRegisterModeMissing = 1ULL << 0;
static const uint64_t kUffdioRegisterModeWp = 1ULL << 1;

/// `UFFDIO_COPY_MODE_DONTWAKE` / `UFFDIO_ZEROPAGE_MODE_DONTWAKE`.
///
/// The handler always wakes (`wake=true` in Rust), so this is only here to
/// document that mode 0 means "wake the faulting thread". Passing DONTWAKE
/// without a later UFFDIO_WAKE leaves the guest blocked forever.
static const uint64_t kUffdioModeDontWake = 1ULL << 0;

struct UffdioRange {
    uint64_t start;
    uint64_t len;
};

struct UffdioRegister {
    UffdioRange range;
    uint64_t mode;
    uint64_t ioctls;
};

struct UffdioCopy {
    uint64_t dst;
    uint64_t src;
    uint64_t len;
    uint64_t mode;
    int64_t copy;
};

struct UffdioZeropage {
    UffdioRange range;
    uint64_t mode;
    int64_t zeropage;
};

/// `struct uffd_msg`. Packed, for the reason in the file header.
struct UffdMsg {
    uint8_t event;
    uint8_t reserved1;
    uint16_t reserved2;
    uint32_t reserved3;
    union {
        struct {
            uint64_t flags;
            uint64_t address;
            uint32_t ptid;
        } pagefault;
        struct {
            uint32_t ufd;
        } fork;
        struct {
            uint64_t from;
            uint64_t to;
            uint64_t len;
        } remap;
        struct {
            uint64_t start;
            uint64_t end;
        } remove;
        struct {
            uint64_t reserved1;
            uint64_t reserved2;
            uint64_t reserved3;
        } reserved;
    } arg;
} __attribute__((packed));

/// `UFFDIO` ioctl numbers.
static const unsigned int kUffdioRegister = _IOWR(0xAA, 0x00, UffdioRegister);
static const unsigned int kUffdioCopy = _IOWR(0xAA, 0x03, UffdioCopy);
static const unsigned int kUffdioZeropage = _IOWR(0xAA, 0x04, UffdioZeropage);

// ---------------------------------------------------------------------------
// Wrapper
// ---------------------------------------------------------------------------

/// Rust `userfaultfd::Event`, narrowed to what `serve_pf` matches on.
struct UffdEvent {
    enum Kind {
        /// `Event::Pagefault { addr, .. }`
        Pagefault,
        /// `Event::Remove { start, end }`
        Remove,
        /// Anything else. Carried rather than dropped so the caller can name
        /// the event it refused, as Rust's catch-all arm does.
        Unsupported
    };

    Kind kind;
    /// Fault address, for `Pagefault`. Not page-aligned by the kernel.
    uint64_t addr;
    /// Removed range `[start, end)`, for `Remove`.
    uint64_t start;
    uint64_t end;
    /// Raw `uffd_msg.event`, for the `Unsupported` message.
    uint8_t raw_event;

    UffdEvent() : kind(Unsupported), addr(0), start(0), end(0), raw_event(0) {}
};

/// The outcome of a UFFDIO_COPY / UFFDIO_ZEROPAGE.
///
/// This is a value rather than an `Expected` because the retry decision needs
/// all three facts at once: how many bytes moved, which errno, and whether the
/// kernel reported a partial transfer. Collapsing it to a Result would force
/// the caller to re-derive the errno from a string.
struct UffdIoResult {
    /// Bytes transferred. May be less than requested even on success.
    int64_t bytes;
    /// 0 when the operation reported no error.
    int error;

    UffdIoResult() : bytes(0), error(0) {}
};

/// Rust `userfaultfd::Uffd`.
///
/// Owns the descriptor. The handler never *creates* one of these: Firecracker
/// creates the uffd, registers the guest regions and passes the descriptor
/// over a Unix socket, so `FromRawFd` is the real constructor and `Create` is
/// only for tests that need a uffd of their own.
class Uffd {
 public:
    /// Takes ownership of `fd`.
    explicit Uffd(int fd) : fd_(fd) {}
    ~Uffd();

    /// Rust `Uffd::from_raw_fd`.
    static Uffd* FromRawFd(int fd) { return new Uffd(fd); }

    /// `userfaultfd(2)` + the UFFDIO_API handshake.
    ///
    /// Not used by the restore path -- see the class comment -- but required
    /// to test the handler without a live Firecracker.
    static core::Expected<Uffd*, std::string> Create(bool non_blocking);

    int fd() const { return fd_; }

    /// Releases ownership without closing.
    int Release();

    /// Rust `uffd.register(start, len)` with MODE_MISSING.
    core::Expected<core::Unit, std::string> Register(uint64_t start, uint64_t len);

    /// Rust `uffd.read_event()`.
    ///
    /// Returns `nullopt` when the descriptor is non-blocking and no event is
    /// queued (EAGAIN), which is a normal poll outcome and not an error.
    core::Expected<core::Optional<UffdEvent>, std::string> ReadEvent();

    /// Rust `uffd.copy(src, dst, len, wake)`.
    ///
    /// `dst` must be page-aligned. Never returns a failure for EAGAIN or
    /// EEXIST; those are reported through `UffdIoResult::error` because the
    /// caller's loop treats them as retry and success respectively.
    UffdIoResult Copy(const void* src, uint64_t dst, std::size_t len, bool wake);

    /// Rust `uffd.zeropage(dst, len, wake)`.
    UffdIoResult ZeroPage(uint64_t dst, std::size_t len, bool wake);

    core::Expected<core::Unit, std::string> Close();

 private:
    Uffd(const Uffd&);
    Uffd& operator=(const Uffd&);

    int fd_;
};

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_UFFD_H_
