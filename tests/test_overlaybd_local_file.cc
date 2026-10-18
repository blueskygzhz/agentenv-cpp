// SPDX-License-Identifier: MIT
// Tests for LocalFile, the first concrete VirtualFile.
// Rust: storage/overlaybd/src/backend/local.rs
#include "microtest.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/local_file.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

/// A scratch path that does not exist yet, removed on scope exit.
class TempPath {
 public:
    TempPath() {
        char tmpl[] = "/tmp/agentenv-lf-XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) ::close(fd);
        path_ = tmpl;
        ::unlink(path_.c_str());
    }
    ~TempPath() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    const std::string& Path() const { return path_; }

 private:
    std::string path_;
};

std::string Read(const std::shared_ptr<LocalFile>& f, uint64_t off, size_t len) {
    core::Expected<ByteBuffer, std::string> r = f->ReadAt(off, len);
    if (!r.ok()) return std::string("<err>");
    return std::string(r.value().begin(), r.value().end());
}

size_t Write(const std::shared_ptr<LocalFile>& f, uint64_t off, const std::string& s) {
    core::Expected<size_t, std::string> r =
        f->WriteAt(off, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return r.ok() ? r.value() : static_cast<size_t>(-1);
}

}  // namespace

// ============================================================
// alignment helpers
// ============================================================

MT_TEST(align_down_and_up_agree_on_an_already_aligned_value) {
    MT_EXPECT_EQ(static_cast<long long>(AlignDown(512, 512)), 512LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignUp(512, 512)), 512LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignDown(0, 512)), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignUp(0, 512)), 0LL);
}

MT_TEST(align_rounds_in_the_right_directions) {
    MT_EXPECT_EQ(static_cast<long long>(AlignDown(513, 512)), 512LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignUp(513, 512)), 1024LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignDown(1, 512)), 0LL);
    MT_EXPECT_EQ(static_cast<long long>(AlignUp(1, 512)), 512LL);
}

MT_TEST(align_up_saturates_instead_of_wrapping_to_zero) {
    const uint64_t max = ~static_cast<uint64_t>(0);
    // Wrapping here would round a value near the maximum down to zero, and
    // the caller would then read or punch from the start of the file rather
    // than the end -- the worst possible substitution.
    const uint64_t got = AlignUp(max - 3, 512);
    MT_EXPECT_TRUE(got != 0);
    MT_EXPECT_TRUE(got >= max - 3 - 512);
}

// ============================================================
// builder
// ============================================================

MT_TEST(open_ro_refuses_to_create) {
    TempPath p;
    // Create disabled, so a missing file is an error rather than a new empty
    // one. A reader that silently got an empty file would report the image as
    // valid-but-blank instead of absent.
    MT_EXPECT_TRUE(!LocalFile::OpenRo(p.Path()).ok());
}

MT_TEST(new_creates_and_truncates) {
    TempPath p;
    {
        core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
        MT_EXPECT_TRUE(f.ok());
        if (!f.ok()) return;
        MT_EXPECT_EQ(static_cast<int>(Write(f.value(), 0, "0123456789")), 10);
    }
    {
        // Reopening with New truncates: the previous contents are gone.
        core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
        MT_EXPECT_TRUE(f.ok());
        if (!f.ok()) return;
        core::Expected<uint64_t, std::string> sz = f.value()->Size();
        MT_EXPECT_TRUE(sz.ok());
        if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 0LL);
    }
}

MT_TEST(open_rw_without_create_requires_the_file_to_exist) {
    TempPath p;
    MT_EXPECT_TRUE(!LocalFile::OpenRw(p.Path(), false).ok());
    MT_EXPECT_TRUE(LocalFile::OpenRw(p.Path(), true).ok());
    MT_EXPECT_TRUE(LocalFile::OpenRw(p.Path(), false).ok());
}

MT_TEST(a_read_only_file_rejects_writes) {
    TempPath p;
    {
        core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
        MT_EXPECT_TRUE(f.ok());
        if (f.ok()) Write(f.value(), 0, "data");
    }
    core::Expected<std::shared_ptr<LocalFile>, std::string> ro = LocalFile::OpenRo(p.Path());
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;
    MT_EXPECT_TRUE(Read(ro.value(), 0, 4) == "data");
    // The descriptor was opened O_RDONLY, so this fails in the kernel rather
    // than being caught here -- which is the point: the mode is enforced by
    // the open, not by a flag this class re-checks.
    MT_EXPECT_EQ(Write(ro.value(), 0, "x"), static_cast<size_t>(-1));
}

// ============================================================
// buffered read / write
// ============================================================

MT_TEST(a_read_past_the_end_comes_back_short_not_failed) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "abc");

    core::Expected<ByteBuffer, std::string> r = f.value()->ReadAt(0, 100);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // Three bytes, and that is a success. The buffer is resized to what was
    // actually read rather than left at the requested length, so the caller
    // cannot mistake uninitialised tail for file contents.
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 3);
}

MT_TEST(a_read_entirely_past_the_end_is_empty) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "abc");
    core::Expected<ByteBuffer, std::string> r = f.value()->ReadAt(1000, 16);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
}

MT_TEST(a_zero_length_read_is_empty_without_touching_the_file) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    core::Expected<ByteBuffer, std::string> r = f.value()->ReadAt(0, 0);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
}

MT_TEST(a_write_past_the_end_extends_the_file_leaving_a_hole) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(Write(f.value(), 4096, "tail")), 4);

    core::Expected<uint64_t, std::string> sz = f.value()->Size();
    MT_EXPECT_TRUE(sz.ok());
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 4100LL);
    // The gap reads as zeros, which is what makes a sparse layer file work at
    // all: an unwritten range is addressable and defined.
    MT_EXPECT_TRUE(Read(f.value(), 0, 4) == std::string("\0\0\0\0", 4));
    MT_EXPECT_TRUE(Read(f.value(), 4096, 4) == "tail");
}

MT_TEST(a_zero_length_write_is_accepted_and_changes_nothing) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    core::Expected<size_t, std::string> r = f.value()->WriteAt(0, 0, 0);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_EQ(r.value(), static_cast<size_t>(0));
    core::Expected<uint64_t, std::string> sz = f.value()->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 0LL);
}

MT_TEST(read_at_into_reports_what_it_got) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "abcdef");

    uint8_t dst[16];
    std::memset(dst, 0xFF, sizeof(dst));
    core::Expected<size_t, std::string> n = f.value()->ReadAtInto(0, dst, sizeof(dst));
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(6));
    // The tail is untouched rather than zero-filled: the count is the
    // caller's signal, and padding would hide a short read.
    MT_EXPECT_EQ(static_cast<int>(dst[6]), 0xFF);
}

MT_TEST(write_at_sync_pwrite_skips_the_alignment_rules) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    const std::string s = "x";
    const uint64_t before = f.value()->WriteCalls();
    core::Expected<size_t, std::string> r = f.value()->WriteAtSyncPwrite(
        1, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    MT_EXPECT_TRUE(r.ok());
    // An offset of 1 and a length of 1 -- neither aligned. Accepted, because
    // this entry point exists for the buffered case where the caller has
    // asserted alignment is irrelevant.
    if (r.ok()) MT_EXPECT_EQ(r.value(), static_cast<size_t>(1));
    MT_EXPECT_EQ(static_cast<long long>(f.value()->WriteCalls() - before), 1LL);
}

// ============================================================
// direct I/O
// ============================================================
//
// Every one of these skips when the filesystem will not take O_DIRECT, which
// overlayfs and tmpfs both refuse. The skip is on the open, not on the
// assertion, so a filesystem that does support it gets the full check.

namespace {

/// Opens with direct I/O, or returns null when the filesystem refuses.
std::shared_ptr<LocalFile> OpenDirect(const std::string& path) {
    core::Expected<std::shared_ptr<LocalFile>, std::string> f =
        LocalFile::Builder().DirectIo(true).Truncate(true).Open(path);
    if (!f.ok()) return std::shared_ptr<LocalFile>();
    return f.value();
}

/// Fills `path` with `n` bytes through a buffered descriptor, so the direct
/// handle has something aligned to read.
void SeedBuffered(const std::string& path, size_t n) {
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(path);
    if (!f.ok()) return;
    std::vector<uint8_t> buf(n);
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(i & 0xFF);
    f.value()->WriteAt(0, &buf[0], n);
}

}  // namespace

MT_TEST(direct_io_reports_itself) {
    TempPath p;
    std::shared_ptr<LocalFile> f = OpenDirect(p.Path());
    if (!f) return;
    MT_EXPECT_TRUE(f->IsDirectIo());
}

MT_TEST(direct_io_refuses_an_unaligned_write_offset_by_name) {
    TempPath p;
    std::shared_ptr<LocalFile> f = OpenDirect(p.Path());
    if (!f) return;

    std::vector<uint8_t> buf(512, 'x');
    core::Expected<size_t, std::string> r = f->WriteAt(1, &buf[0], buf.size());
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // Checked here rather than left to the kernel, which rejects it with
        // a bare EINVAL that is indistinguishable from a dozen other causes.
        MT_EXPECT_TRUE(r.error().find("offset") != std::string::npos);
        MT_EXPECT_TRUE(r.error().find("not aligned") != std::string::npos);
    }
}

MT_TEST(direct_io_refuses_an_unaligned_write_length_by_name) {
    TempPath p;
    std::shared_ptr<LocalFile> f = OpenDirect(p.Path());
    if (!f) return;

    std::vector<uint8_t> buf(100, 'x');
    core::Expected<size_t, std::string> r = f->WriteAt(0, &buf[0], buf.size());
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        MT_EXPECT_TRUE(r.error().find("buf len") != std::string::npos);
        MT_EXPECT_TRUE(r.error().find("not aligned") != std::string::npos);
    }
}

MT_TEST(direct_io_bounces_an_unaligned_buffer_address) {
    TempPath p;
    std::shared_ptr<LocalFile> f = OpenDirect(p.Path());
    if (!f) return;

    // One byte into a larger allocation, so the address is misaligned while
    // the offset and length are not. Rust bounces through an aligned copy
    // here, because the buffer address is routinely not the caller's to
    // control -- it may be the interior of a bigger buffer.
    std::vector<uint8_t> backing(1024 + 512, 0);
    uint8_t* unaligned = &backing[0] + 1;
    for (size_t i = 0; i < 512; ++i) unaligned[i] = static_cast<uint8_t>('A' + (i % 26));

    core::Expected<size_t, std::string> r = f->WriteAt(0, unaligned, 512);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(r.value(), static_cast<size_t>(512));

    core::Expected<ByteBuffer, std::string> back = f->ReadAt(0, 512);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) {
        MT_EXPECT_EQ(static_cast<int>(back.value().size()), 512);
        MT_EXPECT_EQ(static_cast<int>(back.value()[0]), 'A');
        MT_EXPECT_EQ(static_cast<int>(back.value()[25]), 'Z');
    }
}

MT_TEST(direct_io_reads_an_unaligned_range_through_the_aligned_window) {
    TempPath p;
    SeedBuffered(p.Path(), 4096);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;
    std::shared_ptr<LocalFile> f = fo.value();

    // Offset 100, length 50: nothing about this is aligned. The read widens
    // to the enclosing 512-byte window, reads that, and slices out exactly
    // what was asked for -- so the caller never learns the file is O_DIRECT.
    core::Expected<ByteBuffer, std::string> r = f->ReadAt(100, 50);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 50);
    MT_EXPECT_EQ(static_cast<int>(r.value()[0]), 100);
    MT_EXPECT_EQ(static_cast<int>(r.value()[49]), 149);
}

MT_TEST(direct_io_read_crossing_the_end_is_clamped_to_the_file) {
    TempPath p;
    SeedBuffered(p.Path(), 600);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    // Asking for 4096 from offset 500 of a 600-byte file. Clamped to the 100
    // bytes that exist, so the aligned window does not run past the end and
    // come back short for a reason the caller would read as a mid-range EOF.
    core::Expected<ByteBuffer, std::string> r = fo.value()->ReadAt(500, 4096);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 100);
    MT_EXPECT_EQ(static_cast<int>(r.value()[0]), 500 & 0xFF);
}

MT_TEST(direct_io_read_at_into_handles_an_unaligned_destination) {
    TempPath p;
    SeedBuffered(p.Path(), 4096);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    std::vector<uint8_t> backing(1024, 0);
    uint8_t* dst = &backing[0] + 3;  // deliberately misaligned
    core::Expected<size_t, std::string> n = fo.value()->ReadAtInto(7, dst, 33);
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // Routed through the aligned path and copied out: handing an unaligned
    // address to an O_DIRECT read is rejected by the kernel outright.
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(33));
    MT_EXPECT_EQ(static_cast<int>(dst[0]), 7);
    MT_EXPECT_EQ(static_cast<int>(dst[32]), 39);
}

// ============================================================
// metadata and hints
// ============================================================

MT_TEST(truncate_both_grows_and_shrinks) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "0123456789");

    MT_EXPECT_TRUE(f.value()->Truncate(4).ok());
    core::Expected<uint64_t, std::string> sz = f.value()->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 4LL);
    MT_EXPECT_TRUE(Read(f.value(), 0, 8) == "0123");

    MT_EXPECT_TRUE(f.value()->Truncate(8).ok());
    sz = f.value()->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 8LL);
    // Growing fills with zeros, not with whatever the shrink discarded.
    MT_EXPECT_TRUE(Read(f.value(), 4, 4) == std::string("\0\0\0\0", 4));
}

MT_TEST(sync_and_truncate_succeed_here_unlike_the_base_defaults) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    // The base class refuses both, on the grounds that a file which cannot do
    // them must not claim to have. Overriding them is what marks this file as
    // actually able to.
    MT_EXPECT_TRUE(f.value()->Sync().ok());
    MT_EXPECT_TRUE(f.value()->Truncate(0).ok());
}

MT_TEST(seek_data_and_seek_hole_describe_a_sparse_file) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    // A hole at the front, data at 1 MiB.
    Write(f.value(), 1 << 20, "data");

    core::Expected<core::Optional<uint64_t>, std::string> hole = f.value()->SeekHole(0);
    MT_EXPECT_TRUE(hole.ok());
    if (hole.ok() && hole.value().has_value()) {
        MT_EXPECT_EQ(static_cast<long long>(*hole.value()), 0LL);
    }

    core::Expected<core::Optional<uint64_t>, std::string> data = f.value()->SeekData(0);
    MT_EXPECT_TRUE(data.ok());
    // Present and non-zero on a filesystem that tracks extents; absent is
    // also a legal answer, which is why this does not assert a value.
    if (data.ok() && data.value().has_value()) {
        MT_EXPECT_TRUE(*data.value() <= static_cast<uint64_t>(1 << 20));
    }
}

MT_TEST(seek_data_past_the_last_data_answers_absent_not_error) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "abc");

    core::Expected<core::Optional<uint64_t>, std::string> r = f.value()->SeekData(1 << 20);
    // ENXIO means "no data at or after this offset" -- the answer to the
    // question, not a failure to answer it.
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(!r.value().has_value());
}

MT_TEST(discard_frees_space_and_reports_real_failures) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    std::vector<uint8_t> buf(1 << 20, 'x');
    f.value()->WriteAt(0, &buf[0], buf.size());
    MT_EXPECT_TRUE(f.value()->Sync().ok());

    // Zero length returns early: nothing to release, and fallocate would
    // reject it with EINVAL.
    MT_EXPECT_TRUE(f.value()->Discard(0, 0).ok());

    core::Expected<core::Unit, std::string> r = f.value()->Discard(0, 1 << 20);
    if (!r.ok()) {
        // A filesystem without hole punching. Surfaced rather than swallowed,
        // unlike the eviction hints: a caller told this succeeded would
        // believe space was reclaimed when none was.
        MT_EXPECT_TRUE(!r.error().empty());
        return;
    }
    // Size unchanged -- KEEP_SIZE is passed alongside PUNCH_HOLE.
    core::Expected<uint64_t, std::string> sz = f.value()->Size();
    if (sz.ok()) MT_EXPECT_EQ(static_cast<long long>(sz.value()), 1LL << 20);
}

MT_TEST(eviction_hints_succeed_and_do_not_change_what_a_read_returns) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "payload");

    MT_EXPECT_TRUE(f.value()->EvictRange(0, 4096).ok());
    MT_EXPECT_TRUE(f.value()->EvictAll().ok());
    // The whole contract: dropping cache residency changes only whether the
    // next read touches the device. ZFile calls these mid-write and
    // propagates failures, so a platform without posix_fadvise must not break
    // those writes.
    MT_EXPECT_TRUE(Read(f.value(), 0, 7) == "payload");
}

MT_TEST(this_file_supports_xattrs_where_the_base_class_refuses_them) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;

    const std::string name = "user.agentenv_lf";
    const std::string value = "v";
    core::Expected<core::Unit, std::string> set = f.value()->FSetXattr(
        name, reinterpret_cast<const uint8_t*>(value.data()), value.size(), 0);
    if (!set.ok()) return;  // filesystem without user xattrs

    core::Expected<ByteBuffer, std::string> got = f.value()->FGetXattr(name);
    MT_EXPECT_TRUE(got.ok());
    if (got.ok()) {
        MT_EXPECT_TRUE(std::string(got.value().begin(), got.value().end()) == value);
    }

    core::Expected<std::vector<std::string>, std::string> list = f.value()->FListXattr();
    MT_EXPECT_TRUE(list.ok());
    MT_EXPECT_TRUE(f.value()->FRemoveXattr(name).ok());
}

MT_TEST(an_xattr_failure_names_the_attribute) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    core::Expected<ByteBuffer, std::string> r = f.value()->FGetXattr("user.not_set_here");
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // The name is in the message. Without it a failure in a loop over
        // several attributes says only that one of them failed.
        MT_EXPECT_TRUE(r.error().find("user.not_set_here") != std::string::npos);
    }
}

MT_TEST(the_ctx_aware_read_falls_back_and_still_returns_the_right_bytes) {
    TempPath p;
    core::Expected<std::shared_ptr<LocalFile>, std::string> f = LocalFile::New(p.Path());
    MT_EXPECT_TRUE(f.ok());
    if (!f.ok()) return;
    Write(f.value(), 0, "ringless");

    // No override, so this takes the base default and ignores the context.
    // Correct, and the reason it is not wired to the ring is recorded in the
    // header: `Ring::Reap` drains every completion, so a read reaping its own
    // would swallow the ublk queue worker's and wedge the device.
    IoCtx ctx;
    core::Expected<ByteBuffer, std::string> r = f.value()->ReadAtWithCtx(ctx, 0, 8);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) {
        MT_EXPECT_TRUE(std::string(r.value().begin(), r.value().end()) == "ringless");
    }
}


// ============================================================
// direct read boundaries
// ============================================================
//
// The four guards in the aligned-read path (`offset >= size`, the clamp of
// `expected` to what the file holds, `got <= head`, and the `min` on `tail`)
// are mutually masking on the happy path: remove any one and the others still
// produce the right answer. They stop being redundant when the file shrinks
// between the `fstat` and the `pread`, or when the read comes back shorter
// than the size implied -- neither of which a single-threaded test can stage.
// What *is* testable is that the pair covering end-of-file holds jointly, so
// these read a direct handle at and past the end.

MT_TEST(direct_io_read_exactly_at_the_end_is_empty) {
    TempPath p;
    SeedBuffered(p.Path(), 1024);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    core::Expected<ByteBuffer, std::string> r = fo.value()->ReadAt(1024, 512);
    MT_EXPECT_TRUE(r.ok());
    // Empty, not an error and not a short read of padding. Without the
    // `offset >= size` guard the available length underflows and the aligned
    // window is computed from a bogus size.
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());
}

MT_TEST(direct_io_read_far_past_the_end_is_empty) {
    TempPath p;
    SeedBuffered(p.Path(), 1024);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    // Unaligned and well past the end, so the head padding is non-zero and
    // the read returns nothing: this is the case `got <= head` exists for
    // once the size guard is gone.
    core::Expected<ByteBuffer, std::string> r = fo.value()->ReadAt(1024 * 1024 + 7, 50);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_TRUE(r.value().empty());

    core::Expected<size_t, std::string> n = fo.value()->ReadAtInto(1024 * 1024 + 7, 0, 0);
    MT_EXPECT_TRUE(n.ok());
    if (n.ok()) MT_EXPECT_EQ(n.value(), static_cast<size_t>(0));
}

MT_TEST(direct_io_read_of_the_last_partial_block) {
    TempPath p;
    // Not a multiple of 512, so the final block is partial and the aligned
    // window necessarily extends past the end of the file.
    SeedBuffered(p.Path(), 1000);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    core::Expected<ByteBuffer, std::string> r = fo.value()->ReadAt(900, 512);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // 100 bytes, not 512: clamped to what exists rather than padded out to
    // the aligned window the kernel was actually asked for.
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 100);
    MT_EXPECT_EQ(static_cast<int>(r.value()[0]), 900 & 0xFF);
    MT_EXPECT_EQ(static_cast<int>(r.value()[99]), 999 & 0xFF);
}

MT_TEST(direct_io_read_of_a_single_byte_at_an_unaligned_offset) {
    TempPath p;
    SeedBuffered(p.Path(), 2048);
    core::Expected<std::shared_ptr<LocalFile>, std::string> fo =
        LocalFile::Builder().DirectIo(true).Create(false).Open(p.Path());
    if (!fo.ok()) return;

    // head = 511, the largest it can be: the aligned window starts a full
    // block before the byte asked for.
    core::Expected<ByteBuffer, std::string> r = fo.value()->ReadAt(1023, 1);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(r.value().size()), 1);
    MT_EXPECT_EQ(static_cast<int>(r.value()[0]), 1023 & 0xFF);
}

MT_TEST(discard_reports_a_failure_rather_than_claiming_success) {
    TempPath p;
    {
        core::Expected<std::shared_ptr<LocalFile>, std::string> w = LocalFile::New(p.Path());
        MT_EXPECT_TRUE(w.ok());
        if (!w.ok()) return;
        std::vector<uint8_t> buf(8192, 'x');
        w.value()->WriteAt(0, &buf[0], buf.size());
        w.value()->Sync();
    }
    // A read-only descriptor: `fallocate` refuses it with EBADF. This is the
    // distinction against the eviction hints -- a discard that did not happen
    // must not report success, or the caller believes space was reclaimed.
    core::Expected<std::shared_ptr<LocalFile>, std::string> ro = LocalFile::OpenRo(p.Path());
    MT_EXPECT_TRUE(ro.ok());
    if (!ro.ok()) return;

    core::Expected<core::Unit, std::string> r = ro.value()->Discard(0, 4096);
    MT_EXPECT_TRUE(!r.ok());
    if (!r.ok()) {
        // The range is named, so a failure inside a loop over many ranges
        // says which one.
        MT_EXPECT_TRUE(r.error().find("discard") != std::string::npos);
        MT_EXPECT_TRUE(r.error().find("4096") != std::string::npos);
    }
}

MT_MAIN
