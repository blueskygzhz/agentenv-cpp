// SPDX-License-Identifier: MIT
// Tests for the VirtualFile abstraction.
// Rust: storage/overlaybd/src/io/virtual_file.rs.
//
// Most of this file is about the *defaults*, because they are where the
// contract lives. The trait has three pure virtuals and sixteen provided
// methods, and those sixteen fall into three groups that behave differently
// on purpose -- refusing, succeeding as a no-op, and answering "unknown".
// Collapsing any group into another is silent: a `sync` that reports success
// without syncing, or an `evict` that reports failure for doing exactly what
// a hint permits.
#include "microtest.h"

#include <cstring>
#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/virtual_file.h"

using namespace agentenv::storage::overlaybd;
namespace util = agentenv::storage::util;
namespace core = agentenv::core;

namespace {

/// The three required methods over an in-memory buffer, with every default
/// left alone. Everything below exercises the base class through it.
class MemFile : public VirtualFile {
 public:
    MemFile() : short_by_(0), fail_writes_(false) {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override {
        ++reads_;
        last_read_len_ = len;
        if (offset >= data_.size()) return ByteBuffer();
        const size_t avail = data_.size() - static_cast<size_t>(offset);
        size_t take = avail < len ? avail : len;
        // Lets a test force a short read without arranging an exact EOF.
        if (short_by_ > 0 && take > short_by_) take -= short_by_;
        return ByteBuffer(data_.begin() + static_cast<size_t>(offset),
                          data_.begin() + static_cast<size_t>(offset) + take);
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) override {
        if (fail_writes_) {
            return core::make_unexpected(std::string("disk on fire"));
        }
        if (data_.size() < offset + len) data_.resize(static_cast<size_t>(offset) + len);
        if (len > 0) std::memcpy(&data_[static_cast<size_t>(offset)], data, len);
        last_write_len_ = len;
        // A short write is a legal result from this method; the writer above
        // is what decides it is fatal.
        return short_by_ > 0 && len > short_by_ ? len - short_by_ : len;
    }

    core::Expected<uint64_t, std::string> Size() override {
        return static_cast<uint64_t>(data_.size());
    }

    void Fill(const std::string& s) {
        data_.assign(s.begin(), s.end());
    }
    std::string Contents() const {
        return std::string(data_.begin(), data_.end());
    }
    void ShortBy(size_t n) { short_by_ = n; }
    void FailWrites() { fail_writes_ = true; }
    int Reads() const { return reads_; }
    size_t LastReadLen() const { return last_read_len_; }

 private:
    std::vector<uint8_t> data_;
    size_t short_by_;
    bool fail_writes_;
    int reads_ = 0;
    size_t last_read_len_ = 0;
    size_t last_write_len_ = 0;
};

/// Records whether the io context survived the trip through the defaults.
class CtxProbe : public MemFile {
 public:
    core::Expected<ByteBuffer, std::string> ReadAtWithCtx(const IoCtx& ctx, uint64_t off,
                                                           size_t len) override {
        saw_ctx_ = !ctx.Empty();
        ++ctx_reads_;
        return MemFile::ReadAt(off, len);
    }
    bool SawCtx() const { return saw_ctx_; }
    int CtxReads() const { return ctx_reads_; }

 private:
    bool saw_ctx_ = false;
    int ctx_reads_ = 0;
};

}  // namespace

// ============================================================
// the three default groups
// ============================================================

MT_TEST(operations_that_would_lose_data_are_refused_not_faked) {
    MemFile f;
    // Each of these would be a lie if it returned success: the caller would
    // believe its data is durable, its file is shorter, or its space is
    // reclaimed, with nothing to tell it otherwise.
    MT_EXPECT_TRUE(!f.Truncate(0).ok());
    MT_EXPECT_TRUE(!f.Sync().ok());
    MT_EXPECT_TRUE(!f.Discard(0, 4096).ok());

    // Rust's exact wording, so a failure here is greppable against the Rust
    // tree rather than only recognisable by shape.
    MT_EXPECT_TRUE(f.Truncate(0).error() == std::string("truncate is not supported"));
    MT_EXPECT_TRUE(f.Sync().error() == std::string("sync is not supported"));
    MT_EXPECT_TRUE(f.Discard(0, 1).error() == std::string("discard is not supported"));
}

MT_TEST(cache_hints_succeed_by_doing_nothing) {
    MemFile f;
    // The opposite of the group above, and deliberately so. These are hints
    // about cache residency; a file that caches nothing has already satisfied
    // them. Failing would force every caller to handle an error meaning
    // "nothing happened and nothing needed to".
    MT_EXPECT_TRUE(f.EvictRange(0, 4096).ok());
    MT_EXPECT_TRUE(f.EvictAll().ok());
}

MT_TEST(hole_queries_answer_unknown_rather_than_failing) {
    MemFile f;
    // A third shape: the call succeeds but carries no answer. A file that
    // cannot introspect its own sparseness has not failed -- it has nothing
    // to report, and the caller treats the range as data.
    core::Expected<core::Optional<uint64_t>, std::string> d = f.SeekData(0);
    MT_EXPECT_TRUE(d.ok());
    MT_EXPECT_TRUE(!d.value().has_value());

    core::Expected<core::Optional<uint64_t>, std::string> h = f.SeekHole(0);
    MT_EXPECT_TRUE(h.ok());
    MT_EXPECT_TRUE(!h.value().has_value());
}

MT_TEST(xattrs_are_refused_rather_than_answered_with_emptiness) {
    MemFile f;
    // An empty buffer would read as "the attribute is set and blank", and an
    // empty list as "no attributes set" -- both different claims from "this
    // file cannot have attributes".
    MT_EXPECT_TRUE(!f.FGetXattr("user.x").ok());
    MT_EXPECT_TRUE(!f.FListXattr().ok());
    const uint8_t v[1] = {1};
    MT_EXPECT_TRUE(!f.FSetXattr("user.x", v, 1, 0).ok());
    MT_EXPECT_TRUE(!f.FRemoveXattr("user.x").ok());
}

// ============================================================
// read_at_into
// ============================================================

MT_TEST(read_at_into_copies_what_it_got) {
    MemFile f;
    f.Fill("startup-pack");

    uint8_t dst[12];
    std::memset(dst, 0, sizeof(dst));
    core::Expected<size_t, std::string> n = f.ReadAtInto(0, dst, sizeof(dst));
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(12));
    MT_EXPECT_TRUE(std::string(reinterpret_cast<char*>(dst), 12) == "startup-pack");
}

MT_TEST(a_short_read_is_reported_not_padded) {
    MemFile f;
    f.Fill("abc");

    uint8_t dst[8];
    std::memset(dst, 0xFF, sizeof(dst));
    core::Expected<size_t, std::string> n = f.ReadAtInto(0, dst, sizeof(dst));
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    // Three bytes, not eight. Copying the full length would hand back
    // whatever the caller's buffer already held and call it file contents.
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(3));
    MT_EXPECT_EQ(static_cast<int>(dst[3]), 0xFF);  // untouched
}

MT_TEST(reading_past_the_end_yields_nothing_and_is_not_an_error) {
    MemFile f;
    f.Fill("abc");
    uint8_t dst[4];
    core::Expected<size_t, std::string> n = f.ReadAtInto(100, dst, sizeof(dst));
    MT_EXPECT_TRUE(n.ok());
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(0));
}

MT_TEST(an_empty_read_does_not_touch_the_file) {
    MemFile f;
    f.Fill("abc");
    uint8_t dst[1];
    core::Expected<size_t, std::string> n = f.ReadAtInto(0, dst, 0);
    MT_EXPECT_TRUE(n.ok());
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(0));
    // Rust returns early rather than issuing the read. A zero-length read is
    // not an error, and submitting one costs a round trip to copy nothing.
    MT_EXPECT_EQ(f.Reads(), 0);
}

// ============================================================
// io context propagation
// ============================================================

MT_TEST(the_default_ctx_read_falls_back_to_the_plain_one) {
    MemFile f;
    f.Fill("abc");
    // The base default drops the context, which is what lets an
    // implementation that cannot use a ring stay correct.
    util::IoRingConfig cfg;
    IoCtx empty;
    core::Expected<ByteBuffer, std::string> r = f.ReadAtWithCtx(empty, 0, 3);
    MT_EXPECT_TRUE(r.ok());
    if (r.ok()) MT_EXPECT_EQ(r.value().size(), static_cast<size_t>(3));
}

MT_TEST(read_at_into_with_ctx_routes_through_the_ctx_aware_read) {
    CtxProbe f;
    f.Fill("abcdef");

    // A non-null ring is not needed to prove routing; what matters is that
    // the override was reached at all.
    IoCtx ctx(reinterpret_cast<util::Ring*>(0x1));
    uint8_t dst[6];
    core::Expected<size_t, std::string> n = f.ReadAtIntoWithCtx(ctx, 0, dst, sizeof(dst));
    MT_EXPECT_TRUE(n.ok());
    if (!n.ok()) return;
    MT_EXPECT_EQ(n.value(), static_cast<size_t>(6));
    // Through `ReadAtWithCtx`, not `ReadAt`. This is what lets an
    // implementation override one method and have both variants honour the
    // ring -- routing through the plain read would silently drop it.
    MT_EXPECT_EQ(f.CtxReads(), 1);
    MT_EXPECT_TRUE(f.SawCtx());
}

MT_TEST(an_empty_context_is_a_valid_context) {
    CtxProbe f;
    f.Fill("abc");
    IoCtx none;
    MT_EXPECT_TRUE(none.Empty());
    // "No particular ring" is the common case, not an error: a caller with no
    // ring of its own still goes down the same path.
    uint8_t dst[3];
    MT_EXPECT_TRUE(f.ReadAtIntoWithCtx(none, 0, dst, 3).ok());
    MT_EXPECT_TRUE(!f.SawCtx());
}

// ============================================================
// VirtualFileWriter
// ============================================================

MT_TEST(the_writer_uses_32k_buffers_by_default) {
    std::shared_ptr<VirtualFile> f(new MemFile());
    VirtualFileWriter w(f);
    MT_EXPECT_EQ(w.BufferSize(), kVirtualFileWriterBufSize);
    MT_EXPECT_EQ(w.BufferSize(), static_cast<size_t>(32 * 1024));

    core::Expected<util::CompactBuffer, std::string> buf = w.AllocBuffer();
    MT_EXPECT_TRUE(buf.ok());
    if (!buf.ok()) return;
    MT_EXPECT_EQ(buf.value().size(), static_cast<size_t>(32 * 1024));
    // Zeroed, matching Rust's `vec![0u8; n]`: a caller that fills part of the
    // buffer and submits the whole thing writes zeroes rather than whatever
    // the allocator last left there.
    MT_EXPECT_EQ(static_cast<int>(buf.value()[0]), 0);
    MT_EXPECT_EQ(static_cast<int>(buf.value()[32 * 1024 - 1]), 0);
}

MT_TEST(writing_more_than_was_filled_is_refused) {
    std::shared_ptr<VirtualFile> f(new MemFile());
    VirtualFileWriter w(f, 16);

    util::CompactBuffer small(4, 0);
    // Refused rather than clamped to the buffer: a caller asking to write
    // more than it filled has miscounted, and writing the shorter amount
    // would commit a silently truncated record.
    core::Expected<core::Unit, std::string> r = w.Write(small, 0, 8);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("exceed buffer range") != std::string::npos);
}

MT_TEST(a_partially_filled_buffer_may_be_submitted) {
    std::shared_ptr<MemFile> mem(new MemFile());
    VirtualFileWriter w(mem, 16);

    util::CompactBuffer buf(16, 0);
    buf[0] = 'h'; buf[1] = 'i';
    // `len` shorter than the buffer is the normal way to submit a partial
    // fill, and must not write the unused tail.
    MT_EXPECT_TRUE(w.Write(buf, 0, 2).ok());
    MT_EXPECT_TRUE(mem->Contents() == std::string("hi"));
}

MT_TEST(a_short_write_is_fatal_to_the_writer) {
    std::shared_ptr<MemFile> mem(new MemFile());
    mem->ShortBy(2);
    VirtualFileWriter w(mem, 64);

    util::CompactBuffer buf(8, 0);
    core::Expected<core::Unit, std::string> r = w.Write(buf, 0, 8);
    MT_EXPECT_TRUE(!r.ok());
    // A short write is a legitimate result from `WriteAt` but a failure here:
    // this writer lays out a format, and a record that only partly landed
    // leaves the file unparseable with nothing recording where.
    MT_EXPECT_TRUE(r.error().find("short write") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("expected 8") != std::string::npos);
    MT_EXPECT_TRUE(r.error().find("got 6") != std::string::npos);
}

MT_TEST(write_all_at_goes_straight_through) {
    std::shared_ptr<MemFile> mem(new MemFile());
    // A buffer size far below the payload. The base class would chunk; this
    // override writes the caller's slice in one call, so a small metadata
    // write does not get copied through an allocated buffer.
    VirtualFileWriter w(mem, 4);

    const std::string payload = "header-and-trailer";
    MT_EXPECT_TRUE(w.WriteAllAt(reinterpret_cast<const uint8_t*>(payload.data()),
                                payload.size(), 0)
                       .ok());
    MT_EXPECT_TRUE(mem->Contents() == payload);
}

MT_TEST(write_all_at_reports_a_short_write_too) {
    std::shared_ptr<MemFile> mem(new MemFile());
    mem->ShortBy(1);
    VirtualFileWriter w(mem, 64);
    const std::string payload = "abcd";
    core::Expected<core::Unit, std::string> r =
        w.WriteAllAt(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), 0);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().find("short write") != std::string::npos);
}

MT_TEST(an_underlying_write_failure_is_propagated_verbatim) {
    std::shared_ptr<MemFile> mem(new MemFile());
    mem->FailWrites();
    VirtualFileWriter w(mem, 64);
    util::CompactBuffer buf(4, 0);
    core::Expected<core::Unit, std::string> r = w.Write(buf, 0, 4);
    MT_EXPECT_TRUE(!r.ok());
    // Not rewritten as a short write: the file said why it failed, and
    // replacing that with a length mismatch would lose the reason.
    MT_EXPECT_TRUE(r.error().find("disk on fire") != std::string::npos);
}

MT_TEST(writer_defaults_that_are_not_overridden) {
    std::shared_ptr<VirtualFile> f(new MemFile());
    VirtualFileWriter w(f);
    // Random writes are permitted; a sequential-only destination returns true
    // so the caller orders its writes rather than discovering the constraint
    // on the second one.
    MT_EXPECT_TRUE(!w.RequiresOrderedWrites());
    // `finalize` completes format-level output and explicitly does not imply
    // a flush, so a no-op success is the whole contract.
    MT_EXPECT_TRUE(w.Finalize().ok());
}

MT_TEST(a_zero_length_write_is_accepted_and_writes_nothing) {
    std::shared_ptr<MemFile> mem(new MemFile());
    VirtualFileWriter w(mem, 16);
    util::CompactBuffer buf(16, 0);
    MT_EXPECT_TRUE(w.Write(buf, 0, 0).ok());
    MT_EXPECT_TRUE(w.WriteAllAt(NULL, 0, 0).ok());
    MT_EXPECT_TRUE(mem->Contents().empty());
}

MT_MAIN
