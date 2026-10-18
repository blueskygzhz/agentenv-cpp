// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/sys/{mod,open_flags,page_cache,release_space,
// xattr}.rs
#include "agentenv/storage/overlaybd/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace sys {

SysError SysError::Unsupported(const char* op) {
    SysError e;
    e.kind_ = kUnsupported;
    e.errno_ = 0;
    e.op_ = op;
    return e;
}

SysError SysError::FromErrno(int err) {
    SysError e;
    e.kind_ = kErrno;
    e.errno_ = err;
    e.op_ = 0;
    return e;
}

SysError SysError::Last() {
    return FromErrno(errno);
}

std::string SysError::Message() const {
    if (kind_ == kUnsupported) {
        // Rust's wording.
        return std::string(op_ != 0 ? op_ : "operation") +
               " is not supported on this platform";
    }
    return std::string(std::strerror(errno_));
}

// ---------------------------------------------------------------------------
// to_off_t
// ---------------------------------------------------------------------------

namespace {

/// Rust `to_off_t` — reports `EOVERFLOW` rather than aborting. Only reachable
/// on a target with a 32-bit `off_t`, but the check is kept because the
/// alternative is a silently truncated offset addressing the wrong range.
bool ToOffT(uint64_t value, off_t* out) {
    if (value > static_cast<uint64_t>(((off_t)-1) > 0
                                          ? static_cast<uint64_t>((off_t)-1)
                                          : static_cast<uint64_t>(
                                                (~static_cast<uint64_t>(0)) >>
                                                (64 - (sizeof(off_t) * 8 - 1))))) {
        return false;
    }
    *out = static_cast<off_t>(value);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// open flags
// ---------------------------------------------------------------------------

core::Optional<int> DirectIoOpenFlag() {
#ifdef O_DIRECT
    return core::Optional<int>(O_DIRECT);
#else
    // A Linux without O_DIRECT in its headers is not a platform this stack
    // supports, but reporting absence is better than failing to compile: the
    // caller already handles the macOS-shaped "no open flag" case.
    return core::Optional<int>();
#endif
}

core::Expected<core::Unit, SysError> EnableDirectIo(int /*fd*/) {
    // Nothing to do: `O_DIRECT` was set at open time. Rust keeps this call on
    // every platform so `direct_io(true)` runs the same sequence everywhere.
    return core::Unit();
}

// ---------------------------------------------------------------------------
// page cache
// ---------------------------------------------------------------------------

core::Expected<core::Unit, SysError> EvictPageCache(int fd, uint64_t offset,
                                                      uint64_t len) {
    off_t off = 0, l = 0;
    if (!ToOffT(offset, &off) || !ToOffT(len, &l)) {
        return core::make_unexpected(SysError::FromErrno(EOVERFLOW));
    }
    const int ret = ::posix_fadvise(fd, off, l, POSIX_FADV_DONTNEED);
    if (ret == 0) return core::Unit();
    // `posix_fadvise` returns the error number directly instead of setting
    // `errno`. Reading `errno` here would report whatever the last unrelated
    // syscall left behind.
    return core::make_unexpected(SysError::FromErrno(ret));
}

// ---------------------------------------------------------------------------
// space
// ---------------------------------------------------------------------------

core::Expected<core::Unit, SysError> ReleaseSpaceHint(int fd, uint64_t offset,
                                                        uint64_t len) {
    // Rust returns early: a zero-length discard has nothing to release, and
    // `fallocate` would reject it with EINVAL.
    if (len == 0) return core::Unit();

    // Refused rather than clamped. Clamping would turn a malformed request
    // into a shorter well-formed one and release blocks the caller never
    // named -- which for a hole punch means destroying data it did not ask to
    // discard.
    //
    // Redundant on a 64-bit `off_t`, and provably so: `offset + len`
    // overflowing a u64 forces either `offset` or `len` past `off_t`'s
    // maximum of 2^63-1, so `ToOffT` below rejects the same inputs with the
    // same errno. No test can tell the two checks apart here. Kept because it
    // states the precondition where it belongs rather than leaving a
    // conversion further down to enforce it, and because that conversion
    // stops covering it the moment `off_t` is wider than the offsets.
    if (offset > (~static_cast<uint64_t>(0)) - len) {
        return core::make_unexpected(SysError::FromErrno(EOVERFLOW));
    }

    off_t off = 0, l = 0;
    if (!ToOffT(offset, &off) || !ToOffT(len, &l)) {
        return core::make_unexpected(SysError::FromErrno(EOVERFLOW));
    }

#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    // `KEEP_SIZE` alongside `PUNCH_HOLE`: the logical size must not change,
    // only the allocated extents. Without it the file would shrink and every
    // mapping past the hole would read short.
    const int ret =
        ::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, l);
    if (ret == 0) return core::Unit();
    return core::make_unexpected(SysError::Last());
#else
    // Reachable on Linux: a kernel or libc without hole punching, or a
    // filesystem that rejects it. Reported as unsupported rather than as an
    // errno so the caller can tell "never tried" from "tried and failed".
    return core::make_unexpected(SysError::Unsupported("release_space_hint"));
#endif
}

bool SparseExtentsAreReliable() {
    // Linux only, and that is the whole point of the function existing rather
    // than the callers assuming it.
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// xattr
// ---------------------------------------------------------------------------

namespace {

/// True when `s` is well-formed UTF-8.
///
/// Needed because Rust's `parse_nul_list` rejects a non-UTF-8 name; see there
/// for why that is not a lossy conversion.
bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        size_t extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;  // continuation byte or 5/6-byte form
        }
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; ++k) {
            const uint8_t cc = p[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and out-of-range all decode to a
        // codepoint that the shorter form could have expressed, so they are
        // rejected on the value rather than the byte pattern.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

}  // namespace

core::Expected<std::vector<std::string>, SysError> ParseNulList(const uint8_t* buf,
                                                                  size_t len) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        const bool at_end = (i == len);
        if (!at_end && buf[i] != 0) continue;
        const size_t run = i - start;
        // Empty runs are dropped, which covers both the trailing NUL that
        // terminates the last name and a zero-length buffer.
        if (run > 0) {
            if (!IsValidUtf8(buf + start, run)) {
                // Reported rather than replaced lossily. The surrounding API
                // hands names back as strings and builds a C string from them
                // to read or remove an attribute, so a name with U+FFFD
                // substituted for the original bytes would not round-trip --
                // it would quietly address a different attribute or none at
                // all. Failing is recoverable; fabricating a name is not.
                return core::make_unexpected(SysError::FromErrno(EILSEQ));
            }
            out.push_back(std::string(reinterpret_cast<const char*>(buf + start), run));
        }
        start = i + 1;
    }
    return out;
}

namespace {

/// Rust `read_sized` — the two-phase size query shared by the reading xattr
/// calls.
///
/// `probe` is called with a null buffer to ask for the required length, then
/// with a real one to fill it. The value can change in between, and the two
/// directions need different handling:
///
///   * It **shrank** — the fill succeeds and reports fewer bytes, so the
///     result is truncated to what it reported.
///   * It **grew** — the buffer no longer fits and the fill fails with
///     `ERANGE`. Retrying re-queries the new size.
///
/// Bounded rather than unbounded: a value being rewritten in a tight loop
/// would otherwise spin here forever, and reporting `ERANGE` once several
/// attempts in a row have lost the race is the honest outcome.
template <typename Probe>
core::Expected<std::vector<uint8_t>, SysError> ReadSizedImpl(Probe probe) {
    for (int attempt = 0; attempt < kXattrMaxAttempts; ++attempt) {
        const ssize_t need = probe(NULL, 0);
        if (need < 0) return core::make_unexpected(SysError::Last());
        if (need == 0) return std::vector<uint8_t>();

        std::vector<uint8_t> buf(static_cast<size_t>(need), 0);
        const ssize_t got = probe(&buf[0], buf.size());
        if (got >= 0) {
            const size_t n = static_cast<size_t>(got) < buf.size()
                                 ? static_cast<size_t>(got)
                                 : buf.size();
            buf.resize(n);
            return buf;
        }
        if (errno == ERANGE) continue;  // grew between the two calls
        return core::make_unexpected(SysError::Last());
    }
    return core::make_unexpected(SysError::FromErrno(ERANGE));
}

struct GetProbe {
    int fd;
    const char* name;
    ssize_t operator()(uint8_t* dst, size_t len) const {
        return ::fgetxattr(fd, name, dst, len);
    }
};

struct ListProbe {
    int fd;
    ssize_t operator()(uint8_t* dst, size_t len) const {
        return ::flistxattr(fd, reinterpret_cast<char*>(dst), len);
    }
};

/// Rejects an interior NUL before it reaches the syscall, which would
/// otherwise silently address the truncated name.
bool NameIsClean(const std::string& name) {
    return name.find('\0') == std::string::npos;
}

}  // namespace

core::Expected<std::vector<uint8_t>, SysError> FGetXattr(int fd,
                                                           const std::string& name) {
    if (!NameIsClean(name)) {
        return core::make_unexpected(SysError::FromErrno(EINVAL));
    }
    GetProbe p;
    p.fd = fd;
    p.name = name.c_str();
    return ReadSizedImpl(p);
}

core::Expected<std::vector<std::string>, SysError> FListXattr(int fd) {
    ListProbe p;
    p.fd = fd;
    const core::Expected<std::vector<uint8_t>, SysError> raw = ReadSizedImpl(p);
    if (!raw.ok()) return core::make_unexpected(raw.error());
    if (raw.value().empty()) return std::vector<std::string>();
    return ParseNulList(&raw.value()[0], raw.value().size());
}

core::Expected<core::Unit, SysError> FSetXattr(int fd, const std::string& name,
                                                 const uint8_t* value, size_t len,
                                                 int flags) {
    if (!NameIsClean(name)) {
        return core::make_unexpected(SysError::FromErrno(EINVAL));
    }
    // A null value with a zero length is a legitimate way to set an empty
    // attribute, so it is passed through rather than rejected.
    if (::fsetxattr(fd, name.c_str(), value, len, flags) == 0) return core::Unit();
    return core::make_unexpected(SysError::Last());
}

core::Expected<core::Unit, SysError> FRemoveXattr(int fd, const std::string& name) {
    if (!NameIsClean(name)) {
        return core::make_unexpected(SysError::FromErrno(EINVAL));
    }
    if (::fremovexattr(fd, name.c_str()) == 0) return core::Unit();
    return core::make_unexpected(SysError::Last());
}


namespace {

/// Adapts the function-pointer seam to the callable `ReadSizedImpl` expects.
struct FnProbe {
    XattrProbeFn fn;
    void* ctx;
    ssize_t operator()(uint8_t* dst, size_t len) const { return fn(ctx, dst, len); }
};

}  // namespace

core::Expected<std::vector<uint8_t>, SysError> ReadSizedForTesting(XattrProbeFn probe,
                                                                     void* ctx) {
    FnProbe p;
    p.fn = probe;
    p.ctx = ctx;
    return ReadSizedImpl(p);
}

}  // namespace sys
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
