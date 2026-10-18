// SPDX-License-Identifier: MIT
// Tests for the platform primitives under LocalFile.
// Rust: storage/overlaybd/src/sys/{open_flags,page_cache,release_space,xattr}.rs
#include "microtest.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/sys.h"

namespace sys = agentenv::storage::overlaybd::sys;
namespace core = agentenv::core;

namespace {

/// A scratch file, removed on scope exit.
class TempFile {
 public:
    TempFile() : fd_(-1) {
        char tmpl[] = "/tmp/agentenv-sys-XXXXXX";
        fd_ = ::mkstemp(tmpl);
        path_ = tmpl;
    }
    ~TempFile() {
        if (fd_ >= 0) ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    int Fd() const { return fd_; }
    bool Ok() const { return fd_ >= 0; }
    void Grow(size_t n) {
        std::vector<char> buf(n, 'x');
        ssize_t w = ::pwrite(fd_, &buf[0], n, 0);
        (void)w;
    }

 private:
    int fd_;
    std::string path_;
};

size_t Allocated(int fd) {
    struct stat st;
    if (::fstat(fd, &st) != 0) return 0;
    return static_cast<size_t>(st.st_blocks) * 512;
}

}  // namespace

// ============================================================
// SysError
// ============================================================

MT_TEST(unsupported_is_distinguishable_from_an_errno) {
    sys::SysError unsup = sys::SysError::Unsupported("release_space_hint");
    sys::SysError failed = sys::SysError::FromErrno(EINVAL);

    // The whole reason this is a separate kind rather than another errno:
    // callers disagree about what it means. Eviction treats it as success, a
    // hole punch has to surface it. Folding the two together would force
    // every caller to guess from the number which situation it is in.
    MT_EXPECT_TRUE(unsup.IsUnsupported());
    MT_EXPECT_TRUE(!failed.IsUnsupported());

    // The op name survives into the message, which is the only thing it is
    // for.
    MT_EXPECT_TRUE(unsup.Message().find("release_space_hint") != std::string::npos);
    MT_EXPECT_TRUE(unsup.Message().find("not supported") != std::string::npos);
    MT_EXPECT_EQ(failed.Errno(), EINVAL);
}

MT_TEST(errno_is_captured_at_the_point_of_failure) {
    errno = ENOSPC;
    sys::SysError e = sys::SysError::Last();
    // Captured, not looked up later: any intervening call would overwrite the
    // thread's errno and the report would name an unrelated failure.
    errno = 0;
    MT_EXPECT_EQ(e.Errno(), ENOSPC);
}

// ============================================================
// open flags
// ============================================================

MT_TEST(direct_io_is_an_open_flag_on_this_platform) {
    core::Optional<int> flag = sys::DirectIoOpenFlag();
    MT_EXPECT_TRUE(flag.has_value());
    if (flag.has_value()) MT_EXPECT_EQ(*flag, O_DIRECT);
}

MT_TEST(enabling_direct_io_after_open_is_a_noop_here) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    // Linux got what it needed from the open flag. The call is still made on
    // every platform so `direct_io(true)` runs one sequence everywhere rather
    // than being conditionally skipped.
    MT_EXPECT_TRUE(sys::EnableDirectIo(f.Fd()).ok());
}

// ============================================================
// page cache
// ============================================================

MT_TEST(evicting_a_range_succeeds_on_a_real_file) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    f.Grow(8192);
    MT_EXPECT_TRUE(sys::EvictPageCache(f.Fd(), 0, 4096).ok());
    // Zero length means "through end of file", following the posix_fadvise
    // convention -- not "evict nothing".
    MT_EXPECT_TRUE(sys::EvictPageCache(f.Fd(), 0, 0).ok());
}

MT_TEST(fadvise_errors_are_read_from_the_return_value_not_errno) {
    errno = ENOSPC;  // a stale value the implementation must not pick up
    core::Expected<core::Unit, sys::SysError> r = sys::EvictPageCache(-1, 0, 4096);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // `posix_fadvise` returns the error number directly instead of setting
        // errno. Reading errno here would have reported ENOSPC, which has
        // nothing to do with a bad descriptor.
        MT_EXPECT_EQ(r.error().Errno(), EBADF);
    }
}

// ============================================================
// release space
// ============================================================

MT_TEST(a_zero_length_discard_succeeds_without_a_syscall) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    // Returned early rather than passed down: there is nothing to release,
    // and `fallocate` would reject a zero length with EINVAL -- turning a
    // no-op request into a visible failure. The bad descriptor proves no
    // syscall was attempted.
    MT_EXPECT_TRUE(sys::ReleaseSpaceHint(f.Fd(), 0, 0).ok());
    MT_EXPECT_TRUE(sys::ReleaseSpaceHint(-1, 0, 0).ok());
}

MT_TEST(a_range_that_overflows_is_refused_not_clamped) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    const uint64_t max = ~static_cast<uint64_t>(0);
    core::Expected<core::Unit, sys::SysError> r =
        sys::ReleaseSpaceHint(f.Fd(), max - 10, 1000);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // Clamping would turn a malformed request into a shorter well-formed
        // one and punch a hole the caller never named. For a discard that is
        // data loss, so the call is refused.
        MT_EXPECT_EQ(r.error().Errno(), EOVERFLOW);
        // An overflow is a real rejection, not a missing platform feature.
        MT_EXPECT_TRUE(!r.error().IsUnsupported());
    }
}

MT_TEST(punching_a_hole_frees_blocks_but_keeps_the_size) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    f.Grow(1 << 20);  // 1 MiB of real blocks
    ::fsync(f.Fd());
    const size_t before = Allocated(f.Fd());

    core::Expected<core::Unit, sys::SysError> r = sys::ReleaseSpaceHint(f.Fd(), 0, 1 << 20);
    if (!r.ok() && r.error().IsUnsupported()) return;  // filesystem without it
    MT_EXPECT_TRUE(r.ok());

    struct stat st;
    MT_EXPECT_EQ(::fstat(f.Fd(), &st), 0);
    // KEEP_SIZE is passed alongside PUNCH_HOLE: without it the file would
    // shrink and every mapping past the hole would start reading short.
    MT_EXPECT_EQ(static_cast<long long>(st.st_size), static_cast<long long>(1 << 20));
    if (before > 0) MT_EXPECT_TRUE(Allocated(f.Fd()) < before);
}

// ============================================================
// parse_nul_list
// ============================================================

MT_TEST(the_name_list_splits_on_nul_and_drops_empty_runs) {
    const uint8_t buf[] = "user.a\0user.b\0";
    core::Expected<std::vector<std::string>, sys::SysError> r =
        sys::ParseNulList(buf, sizeof(buf) - 1);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // Two names, not three: the trailing NUL terminates the last name rather
    // than introducing an empty one.
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 2);
    MT_EXPECT_TRUE(r.value()[0] == "user.a");
    MT_EXPECT_TRUE(r.value()[1] == "user.b");
}

MT_TEST(an_empty_list_is_empty_not_an_error) {
    core::Expected<std::vector<std::string>, sys::SysError> r = sys::ParseNulList(0, 0);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
}

MT_TEST(a_non_utf8_name_is_refused_rather_than_mangled) {
    // 0xFF is not a legal lead byte.
    const uint8_t buf[] = {'u', 's', 'e', 'r', '.', 0xFF, 0x00};
    core::Expected<std::vector<std::string>, sys::SysError> r = sys::ParseNulList(buf, 6);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // Not replaced lossily. The surrounding API hands names back as
        // strings and rebuilds a C string from them to read or remove an
        // attribute, so a name with U+FFFD substituted for the original bytes
        // would not round-trip -- it would address a different attribute, or
        // none. Failing is recoverable; fabricating a name is not.
        MT_EXPECT_EQ(r.error().Errno(), EILSEQ);
    }
}

MT_TEST(an_overlong_encoding_is_refused) {
    // C0 80 is a two-byte encoding of NUL -- bit-legal, but decodes to a
    // codepoint the one-byte form expresses. Accepting it would let two
    // different byte strings name the same attribute.
    const uint8_t buf[] = {0xC0, 0x80};
    MT_EXPECT_TRUE(!sys::ParseNulList(buf, 2).ok());

    // A surrogate, which is never valid in UTF-8.
    const uint8_t sur[] = {0xED, 0xA0, 0x80};
    MT_EXPECT_TRUE(!sys::ParseNulList(sur, 3).ok());

    // A truncated multi-byte sequence at the end of the buffer.
    const uint8_t trunc[] = {0xE2, 0x82};
    MT_EXPECT_TRUE(!sys::ParseNulList(trunc, 2).ok());

    // A bare continuation byte, with no lead.
    const uint8_t cont[] = {0x80};
    MT_EXPECT_TRUE(!sys::ParseNulList(cont, 1).ok());
}

MT_TEST(a_valid_multibyte_name_is_kept_byte_for_byte) {
    // U+20AC EURO SIGN, three bytes.
    const uint8_t buf[] = {'a', 0xE2, 0x82, 0xAC, 0x00};
    core::Expected<std::vector<std::string>, sys::SysError> r = sys::ParseNulList(buf, 4);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 1);
    // Four bytes, not four codepoints: the name is handed back as the exact
    // bytes the kernel reported, because that is what has to go back in to
    // address the same attribute.
    MT_EXPECT_EQ(static_cast<int>(r.value()[0].size()), 4);
}

// ============================================================
// xattr round trip
// ============================================================

MT_TEST(an_interior_nul_in_a_name_is_refused_before_the_syscall) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    std::string bad("user.a");
    bad.push_back('\0');
    bad += "ignored";

    const uint8_t v[1] = {1};
    // Rust gets this from `CString::new`, which refuses the same thing.
    // Passing it through would silently truncate at the NUL and address
    // `user.a` while the caller believes it named something longer.
    MT_EXPECT_EQ(sys::FGetXattr(f.Fd(), bad).error().Errno(), EINVAL);
    MT_EXPECT_EQ(sys::FSetXattr(f.Fd(), bad, v, 1, 0).error().Errno(), EINVAL);
    MT_EXPECT_EQ(sys::FRemoveXattr(f.Fd(), bad).error().Errno(), EINVAL);
}

MT_TEST(set_get_list_remove_round_trip) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    const std::string name = "user.agentenv";
    const std::string value = "startup-pack";

    core::Expected<core::Unit, sys::SysError> set = sys::FSetXattr(
        f.Fd(), name, reinterpret_cast<const uint8_t*>(value.data()), value.size(), 0);
    if (!set.ok()) {
        // tmpfs and some filesystems reject user.* xattrs outright; that is a
        // property of the mount, not of this code.
        if (set.error().Errno() == EOPNOTSUPP || set.error().Errno() == ENOTSUP) return;
    }
    MT_EXPECT_TRUE(set.ok());

    core::Expected<std::vector<uint8_t>, sys::SysError> got = sys::FGetXattr(f.Fd(), name);
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) {
        MT_EXPECT_TRUE(std::string(got.value().begin(), got.value().end()) == value);
    }

    core::Expected<std::vector<std::string>, sys::SysError> list = sys::FListXattr(f.Fd());
    MT_EXPECT_TRUE(list.ok());
    if (list.ok()) {
        bool found = false;
        for (size_t i = 0; i < list.value().size(); ++i) {
            if (list.value()[i] == name) found = true;
        }
        MT_EXPECT_TRUE(found);
    }

    MT_EXPECT_TRUE(sys::FRemoveXattr(f.Fd(), name).ok());
    MT_EXPECT_TRUE(!sys::FGetXattr(f.Fd(), name).ok());
}

MT_TEST(an_empty_attribute_value_is_a_value_not_an_absence) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    const std::string name = "user.agentenv_empty";
    core::Expected<core::Unit, sys::SysError> set = sys::FSetXattr(f.Fd(), name, 0, 0, 0);
    if (!set.ok()) {
        if (set.error().Errno() == EOPNOTSUPP || set.error().Errno() == ENOTSUP) return;
    }
    MT_EXPECT_TRUE(set.ok());

    // The probe reports zero required bytes and the early return hands back an
    // empty buffer successfully. Treating a zero-size probe as an error would
    // make a legitimately empty attribute unreadable.
    core::Expected<std::vector<uint8_t>, sys::SysError> got = sys::FGetXattr(f.Fd(), name);
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) MT_EXPECT_TRUE(got.value().empty());
    sys::FRemoveXattr(f.Fd(), name);
}

MT_TEST(listing_a_file_with_no_attributes_is_an_empty_list) {
    TempFile f;
    MT_EXPECT_TRUE(f.Ok());
    core::Expected<std::vector<std::string>, sys::SysError> list = sys::FListXattr(f.Fd());
    MT_EXPECT_TRUE(list.ok());
    if (list.ok()) MT_EXPECT_TRUE(list.value().empty());
}

MT_TEST(a_bad_descriptor_surfaces_as_an_errno_not_as_unsupported) {
    core::Expected<std::vector<uint8_t>, sys::SysError> r = sys::FGetXattr(-1, "user.x");
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().Errno(), EBADF);
        // The distinction the caller relies on: this was attempted and
        // failed, so it must not be swallowed the way a missing platform
        // feature is.
        MT_EXPECT_TRUE(!r.error().IsUnsupported());
    }
}


// ============================================================
// read_sized — the two-phase size query
// ============================================================
//
// These go through the injected probe rather than a real file because the
// behaviour under test is a race against a concurrent attribute writer: the
// value changing size between the probe and the fill. No ordering of real
// syscalls stages that reliably.

namespace {

/// Scripts one probe-then-fill sequence per attempt.
struct ScriptedProbe {
    std::vector<ssize_t> probe_sizes;  ///< what each size query reports
    std::vector<ssize_t> fill_results; ///< what each fill returns (<0 => errno)
    std::vector<int> fill_errnos;
    int probes;
    int fills;

    ScriptedProbe() : probes(0), fills(0) {}

    static ssize_t Call(void* ctx, uint8_t* dst, size_t len) {
        ScriptedProbe* self = static_cast<ScriptedProbe*>(ctx);
        if (dst == 0 && len == 0) {
            const size_t i = static_cast<size_t>(self->probes++);
            return i < self->probe_sizes.size() ? self->probe_sizes[i] : 0;
        }
        const size_t i = static_cast<size_t>(self->fills++);
        const ssize_t r = i < self->fill_results.size() ? self->fill_results[i] : 0;
        if (r < 0) {
            errno = i < self->fill_errnos.size() ? self->fill_errnos[i] : EIO;
            return -1;
        }
        // Fill with a recognisable byte so a truncation bug shows up as
        // trailing zeroes rather than as the right length by accident.
        for (size_t k = 0; k < len && k < static_cast<size_t>(r); ++k) dst[k] = 0x5A;
        return r;
    }
};

}  // namespace

MT_TEST(a_value_that_shrank_between_the_two_calls_is_truncated) {
    ScriptedProbe p;
    p.probe_sizes.push_back(16);   // probe says 16
    p.fill_results.push_back(4);   // writer shrank it; fill reports 4

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // Four bytes, not sixteen. Keeping the buffer at the probed size would
    // hand back twelve trailing zeroes as if they were part of the value --
    // and for an attribute holding a length-prefixed blob, that parses.
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 4);
    MT_EXPECT_EQ(static_cast<int>(r.value()[3]), 0x5A);
    MT_EXPECT_EQ(p.probes, 1);
    MT_EXPECT_EQ(p.fills, 1);
}

MT_TEST(a_value_that_grew_is_retried_with_the_new_size) {
    ScriptedProbe p;
    p.probe_sizes.push_back(4);    // probe says 4
    p.fill_results.push_back(-1);  // writer grew it
    p.fill_errnos.push_back(ERANGE);
    p.probe_sizes.push_back(32);   // re-probe sees the new size
    p.fill_results.push_back(32);

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 32);
    // Re-probed rather than retried with the same buffer: the point of going
    // round again is to learn the size that made the last fill fail.
    MT_EXPECT_EQ(p.probes, 2);
    MT_EXPECT_EQ(p.fills, 2);
}

MT_TEST(a_value_rewritten_in_a_loop_gives_up_rather_than_spinning) {
    ScriptedProbe p;
    for (int i = 0; i < 10; ++i) {
        p.probe_sizes.push_back(4);
        p.fill_results.push_back(-1);
        p.fill_errnos.push_back(ERANGE);
    }

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) MT_EXPECT_EQ(r.error().Errno(), ERANGE);
    // Bounded, not unbounded. A value being rewritten in a tight loop would
    // otherwise hold this thread forever -- and on the ublk queue thread that
    // is the whole device wedged, not one slow read.
    MT_EXPECT_EQ(p.probes, sys::kXattrMaxAttempts);
    MT_EXPECT_EQ(p.fills, sys::kXattrMaxAttempts);
}

MT_TEST(a_zero_size_probe_returns_empty_without_a_fill) {
    ScriptedProbe p;
    p.probe_sizes.push_back(0);

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
    MT_EXPECT_EQ(p.fills, 0);
    // The probe count, not just the fill count, is what pins this down. A
    // fill with a zero-length buffer is indistinguishable from a size query
    // -- in this fake and in the real `fgetxattr`, where a null buffer *is*
    // the size query. So skipping the early return would not register as a
    // fill; it would register as a second probe, and cost a wasted syscall on
    // every read of an empty attribute. It also takes the address of an empty
    // vector's first element, which is undefined.
    MT_EXPECT_EQ(p.probes, 1);
}

MT_TEST(a_failing_size_probe_is_reported_not_retried) {
    ScriptedProbe p;
    p.probe_sizes.push_back(-1);
    errno = ENODATA;

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(!r.ok());
    // One attempt. The retry exists for a value that changed size, not for a
    // probe that failed outright -- an absent attribute would otherwise be
    // asked for four times on every read.
    MT_EXPECT_EQ(p.probes, 1);
}

MT_TEST(a_fill_failure_other_than_erange_is_reported_immediately) {
    ScriptedProbe p;
    p.probe_sizes.push_back(8);
    p.fill_results.push_back(-1);
    p.fill_errnos.push_back(EPERM);

    core::Expected<std::vector<uint8_t>, sys::SysError> r =
        sys::ReadSizedForTesting(&ScriptedProbe::Call, &p);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) MT_EXPECT_EQ(r.error().Errno(), EPERM);
    // Only ERANGE means "try again with a bigger buffer". Retrying anything
    // else would turn one permission failure into four.
    MT_EXPECT_EQ(p.probes, 1);
}

MT_MAIN
