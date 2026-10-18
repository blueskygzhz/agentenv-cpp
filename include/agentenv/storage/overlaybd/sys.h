// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/sys/{mod,open_flags,page_cache,release_space,
// xattr}.rs
//
// Platform primitives that sit under `LocalFile`. Rust compiles a Linux and a
// macOS arm for each; this port carries the Linux arm only, because ublk is a
// Linux interface and nothing above this layer can run anywhere else. The
// `Unsupported` variant is still modelled -- it is reachable on Linux too, from
// filesystems that reject `fallocate`.
#ifndef AGENTENV_STORAGE_OVERLAYBD_SYS_H_
#define AGENTENV_STORAGE_OVERLAYBD_SYS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace sys {

/// Rust `SysError` — failure of a platform primitive.
///
/// `Unsupported` is a distinct kind rather than just another errno because
/// callers legitimately disagree about what it means: page-cache eviction is a
/// hint and degrades to a no-op, while a failed hole punch has to surface.
/// Folding it into an errno would force every caller to guess from the number
/// which of those two situations it is in.
class SysError {
 public:
    enum Kind { kUnsupported, kErrno };

    /// The platform never attempted the operation. `op` is for diagnostics.
    static SysError Unsupported(const char* op);
    /// The syscall was attempted and failed with `err`.
    static SysError FromErrno(int err);
    /// Capture the thread's current `errno`.
    static SysError Last();

    /// Rust `is_unsupported` — true when the platform never tried.
    bool IsUnsupported() const { return kind_ == kUnsupported; }
    int Errno() const { return errno_; }
    std::string Message() const;

 private:
    SysError() : kind_(kErrno), errno_(0), op_(NULL) {}
    Kind kind_;
    int errno_;
    const char* op_;
};

template <typename T>
struct SysResult {
    typedef core::Expected<T, SysError> type;
};

// -- open flags -------------------------------------------------------------

/// Rust `direct_io_open_flag` — the open flag requesting cache-bypassing I/O.
///
/// Present on Linux (`O_DIRECT`). Returned as an optional because Rust's macOS
/// arm has none and expresses it after open instead; keeping the shape means a
/// caller written against it stays correct if that arm is ever added.
core::Optional<int> DirectIoOpenFlag();

/// Rust `enable_direct_io` — finish enabling cache-bypassing I/O after open.
///
/// A no-op on Linux, where `O_DIRECT` already did the work at open time. Kept
/// so the caller's sequence is the same on every platform rather than being
/// conditionally skipped.
SysResult<core::Unit>::type EnableDirectIo(int fd);

// -- page cache -------------------------------------------------------------

/// Rust `evict_page_cache` — hint that the cache for a range can be dropped.
///
/// `len == 0` means "through end of file", following the `posix_fadvise`
/// convention. Advisory: it never changes what a later read returns, only
/// whether that read touches the device.
SysResult<core::Unit>::type EvictPageCache(int fd, uint64_t offset, uint64_t len);

// -- space ------------------------------------------------------------------

/// Rust `release_space_hint` — punch a hole, keeping the logical file size.
///
/// A hint about *space*: it makes no promise the range reads back as zeros,
/// and a range too small to cover a block releases nothing and still succeeds.
/// Real syscall failures do surface, unlike the eviction hint above.
SysResult<core::Unit>::type ReleaseSpaceHint(int fd, uint64_t offset, uint64_t len);

// -- xattr ------------------------------------------------------------------

/// Rust `fgetxattr` / `flistxattr` / `fsetxattr` / `fremovexattr`.
SysResult<std::vector<uint8_t> >::type FGetXattr(int fd, const std::string& name);
SysResult<std::vector<std::string> >::type FListXattr(int fd);
SysResult<core::Unit>::type FSetXattr(int fd, const std::string& name,
                                       const uint8_t* value, size_t len, int flags);
SysResult<core::Unit>::type FRemoveXattr(int fd, const std::string& name);

/// Rust `parse_nul_list` — split the NUL-separated name list from `flistxattr`.
///
/// Exposed for testing; see the implementation for why an invalid name is an
/// error rather than a lossy conversion.
SysResult<std::vector<std::string> >::type ParseNulList(const uint8_t* buf, size_t len);

/// A synthetic stand-in for an xattr syscall, used to drive `read_sized`'s
/// two-phase query from a test.
///
/// Returns what the syscall would: negative on failure with `errno` set,
/// otherwise the required byte count (when `dst` is null) or the number
/// written. A seam is needed because the two behaviours that matter -- the
/// value shrinking or growing between the size query and the fill -- are
/// races against a concurrent writer that no sequence of real syscalls can
/// stage.
typedef ssize_t (*XattrProbeFn)(void* ctx, uint8_t* dst, size_t len);

/// Rust `read_sized`, reachable with an injected probe. Production callers go
/// through `FGetXattr` / `FListXattr`.
core::Expected<std::vector<uint8_t>, SysError> ReadSizedForTesting(XattrProbeFn probe,
                                                                     void* ctx);

/// Rust's `MAX_ATTEMPTS` in `read_sized`.
const int kXattrMaxAttempts = 4;

}  // namespace sys
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_SYS_H_
