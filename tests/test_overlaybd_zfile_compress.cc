// SPDX-License-Identifier: MIT
// Tests for the ZFile *encode* half.
//
// Rust: storage/overlaybd/src/compression/zfile.rs — `zfile_compress`,
//       `ZFileBuilder`, `ZFileCompactWriter`, `WorkerPool`, `compress_segment`.
//
// Almost every test here closes the loop through the already-ported decode
// half: the point of an encoder is that something can read what it wrote, and
// a test that only inspected the encoder's own bookkeeping would pass just as
// well on bytes no reader accepts. Where a test instead asserts on raw bytes,
// it is because the property is about the bytes themselves (the fan-out
// producing identical output, the header flags).
#include "microtest.h"

#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/virtual_file.h"
#include "agentenv/storage/overlaybd/zfile.h"

namespace core = agentenv::core;
namespace obd = agentenv::storage::overlaybd;
namespace obdz = agentenv::storage::overlaybd::zfile;

namespace {

// ============================================================
// probes
// ============================================================

/// An in-memory destination that is deliberately **not** a `LocalFile`.
///
/// Two jobs. It is the only way to inject a write failure, and it is what
/// makes the `CommitCompressedBatch` fast path testable at all: that path is
/// chosen by a `dynamic_cast` to `LocalFile`, so a different implementation
/// takes the ordinary `WriteAt` route and the two can be compared.
class MemoryFile : public obd::VirtualFile {
 public:
    MemoryFile() : fail_after_(-1), writes_(0) {}

    core::Expected<obd::ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) {
        if (offset >= bytes_.size()) return obd::ByteBuffer();
        const size_t avail = static_cast<size_t>(bytes_.size() - offset);
        const size_t n = len < avail ? len : avail;
        return obd::ByteBuffer(bytes_.begin() + static_cast<size_t>(offset),
                               bytes_.begin() + static_cast<size_t>(offset) + n);
    }

    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                 size_t len) {
        ++writes_;
        if (fail_after_ >= 0 && writes_ > fail_after_) {
            return core::make_unexpected(std::string("injected write failure"));
        }
        if (len == 0) return static_cast<size_t>(0);
        const size_t end = static_cast<size_t>(offset) + len;
        if (end > bytes_.size()) bytes_.resize(end, 0);
        std::memcpy(&bytes_[static_cast<size_t>(offset)], data, len);
        return len;
    }

    core::Expected<uint64_t, std::string> Size() {
        return static_cast<uint64_t>(bytes_.size());
    }

    /// Writes succeed until this many have been served, then fail forever.
    void FailAfterWrites(int n) { fail_after_ = n; }
    const std::vector<uint8_t>& Bytes() const { return bytes_; }
    void Fill(const std::vector<uint8_t>& data) { bytes_ = data; }

 private:
    std::vector<uint8_t> bytes_;
    int fail_after_;
    int writes_;
};

// ============================================================
// helpers
// ============================================================

/// Compressible, but not trivially so: a run-heavy pattern with enough
/// variation that LZ4 and ZSTD produce different block lengths, so a test
/// comparing against a fixed size would not pass by accident.
std::vector<uint8_t> SampleData(size_t len) {
    std::vector<uint8_t> out(len, 0);
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>((i / 7) % 251);
    }
    return out;
}

/// Incompressible input, so the encoder's output is *larger* than its input
/// and `MaxDstSize()` is the only thing keeping the write in bounds.
std::vector<uint8_t> RandomishData(size_t len) {
    std::vector<uint8_t> out(len, 0);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < len; ++i) {
        state = state * 1103515245u + 12345u;
        out[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
    }
    return out;
}

obdz::CompressArgs ArgsFor(uint8_t algo, uint8_t verify, size_t workers) {
    obdz::CompressOptions opt;
    opt.algo = algo;
    opt.verify = verify;
    opt.block_size = obdz::CompressOptions::kDefaultBlockSize;
    obdz::CompressArgs args(opt);
    args.workers = workers;
    return args;
}

/// Read the whole logical file back through the decode half.
core::Expected<std::vector<uint8_t>, std::string> ReadBackAll(
    std::shared_ptr<obd::VirtualFile> encoded) {
    const core::Expected<std::shared_ptr<obdz::ZFileRO>, std::string> ro =
        obdz::ZFileOpenRo(encoded, false);
    if (!ro.ok()) return core::make_unexpected(ro.error());

    const uint64_t total = ro.value()->OriginalSize();
    std::vector<uint8_t> out(static_cast<size_t>(total), 0);
    if (total == 0) return out;

    size_t done = 0;
    while (done < out.size()) {
        const core::Expected<size_t, std::string> n =
            ro.value()->Pread(&out[done], out.size() - done, done);
        if (!n.ok()) return core::make_unexpected(n.error());
        if (n.value() == 0) {
            return core::make_unexpected(std::string("short read from zfile"));
        }
        done += n.value();
    }
    return out;
}

std::string TempPath(const char* name) {
    return std::string("/tmp/agentenv-zfile-test-") + name + "-" +
           std::to_string(static_cast<long long>(::getpid()));
}

/// A writable local file, which is what selects the sync-pwrite fast path.
/// `New` rather than `OpenRw(path, false)`: the latter does not create.
std::shared_ptr<obd::LocalFile> OpenLocal(const std::string& path) {
    ::unlink(path.c_str());
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> f =
        obd::LocalFile::New(path);
    if (!f.ok()) return std::shared_ptr<obd::LocalFile>();
    return f.value();
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================
// zfile_compress: the whole-file path
// ============================================================

MT_TEST(compressed_output_round_trips_for_every_codec_and_verify_setting) {
    // Rust `test_verify_compression_matrix`. The matrix matters because
    // `verify` changes the on-disk block layout (each block grows a salted
    // CRC trailer that the jump table has to account for), so a codec that
    // round-trips with verify off can still be broken with it on.
    const uint8_t algos[2] = {obdz::CompressOptions::kLz4, obdz::CompressOptions::kZstd};
    const uint8_t verifies[2] = {0, 1};
    const std::vector<uint8_t> data = SampleData(40000);

    for (int a = 0; a < 2; ++a) {
        for (int v = 0; v < 2; ++v) {
            std::shared_ptr<MemoryFile> src(new MemoryFile);
            src->Fill(data);
            std::shared_ptr<MemoryFile> dst(new MemoryFile);

            const core::Expected<core::Unit, std::string> done =
                obdz::ZFileCompress(src, dst, ArgsFor(algos[a], verifies[v], 1));
            MT_EXPECT_TRUE(done.ok());
            if (!done.ok()) continue;

            // Actually compressed, not merely copied.
            MT_EXPECT_TRUE(dst->Bytes().size() < data.size());

            const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
            MT_EXPECT_TRUE(back.ok());
            if (!back.ok()) continue;
            MT_EXPECT_EQ(static_cast<long long>(back.value().size()),
                         static_cast<long long>(data.size()));
            MT_EXPECT_TRUE(back.value() == data);
        }
    }
}

MT_TEST(incompressible_input_round_trips) {
    // The interesting case for `MaxDstSize`: LZ4 emits *more* than a block
    // for random input, so a destination sized at the block size would
    // overflow here and nowhere else.
    const std::vector<uint8_t> data = RandomishData(20000);
    std::shared_ptr<MemoryFile> src(new MemoryFile);
    src->Fill(data);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    const core::Expected<core::Unit, std::string> done =
        obdz::ZFileCompress(src, dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 1));
    MT_EXPECT_TRUE(done.ok());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) MT_EXPECT_TRUE(back.value() == data);
}

MT_TEST(an_empty_source_compresses_but_the_result_is_not_a_readable_zfile) {
    // Recording what the format does, not what would be convenient: a zfile
    // with no blocks has an empty jump table, and the reader rejects that
    // outright -- `ensure!(nindex != 0, "empty index is not a valid zfile")`
    // in Rust's `zfile_open_ro`, matched here. So compression of an empty
    // source *succeeds* and produces a file nothing can open.
    //
    // This is why it is asserted rather than left undefined: a caller that
    // compresses a possibly-empty input has to handle the empty case itself,
    // and a test claiming the round trip works would hide that.
    std::shared_ptr<MemoryFile> src(new MemoryFile);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    const core::Expected<core::Unit, std::string> done =
        obdz::ZFileCompress(src, dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(done.ok());
    // A header and a trailer were still written, so the result is not empty.
    MT_EXPECT_TRUE(dst->Bytes().size() >= 2 * obdz::kHeaderTrailerSpace);

    const core::Expected<std::shared_ptr<obdz::ZFileRO>, std::string> ro =
        obdz::ZFileOpenRo(dst, false);
    MT_EXPECT_TRUE(!ro.ok());
    if (!ro.ok()) MT_EXPECT_TRUE(Contains(ro.error(), "empty index is not a valid zfile"));

    // It does still carry a zfile header, which is what `IsZFile` reports on.
    // The two answers differ on purpose: the probe asks "is this the format",
    // the open asks "can this be read".
    const core::Expected<int, std::string> probe = obdz::IsZFile(dst.get());
    MT_EXPECT_TRUE(probe.ok());
    if (probe.ok()) MT_EXPECT_EQ(static_cast<long long>(probe.value()), 1LL);
}

MT_TEST(compressed_output_is_recognised_as_a_zfile) {
    // This is the gate the recontainerize path depends on: a layer it just
    // compressed must probe as zfile, or a re-publish would compress it
    // again, and the "already compressed, pass through" branch would be
    // unreachable.
    const std::vector<uint8_t> data = SampleData(9000);
    std::shared_ptr<MemoryFile> src(new MemoryFile);
    src->Fill(data);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    MT_EXPECT_TRUE(obdz::ZFileCompress(src, dst, ArgsFor(obdz::CompressOptions::kZstd, 1, 1)).ok());

    const core::Expected<int, std::string> probe = obdz::IsZFile(dst.get());
    MT_EXPECT_TRUE(probe.ok());
    if (probe.ok()) MT_EXPECT_EQ(static_cast<long long>(probe.value()), 1LL);

    // ...and the uncompressed source does not, which is what makes the
    // assertion above mean something.
    const core::Expected<int, std::string> raw = obdz::IsZFile(src.get());
    MT_EXPECT_TRUE(raw.ok());
    if (raw.ok()) MT_EXPECT_EQ(static_cast<long long>(raw.value()), 0LL);
}

MT_TEST(a_verified_file_passes_the_crc_validation_check) {
    // Cross-checks the salted per-block CRC against the *reader's*
    // independent recomputation. The encoder and the validator derive the
    // value from the same salt, so a wrong salt or a CRC taken over the
    // uncompressed bytes fails here rather than silently producing a file
    // that only this codebase can read.
    const std::vector<uint8_t> data = SampleData(30000);
    std::shared_ptr<MemoryFile> src(new MemoryFile);
    src->Fill(data);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    MT_EXPECT_TRUE(obdz::ZFileCompress(src, dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 1)).ok());
    MT_EXPECT_TRUE(obdz::ZFileValidationCheck(dst).ok());
}

MT_TEST(overwrite_header_marks_the_header_it_rewrote) {
    // Rust `test_overwrite_header_path`. The flag is how a reader tells a
    // header rewritten after the index was known from the placeholder written
    // before any data existed.
    const std::vector<uint8_t> data = SampleData(9000);

    std::shared_ptr<MemoryFile> plain_dst(new MemoryFile);
    {
        std::shared_ptr<MemoryFile> src(new MemoryFile);
        src->Fill(data);
        MT_EXPECT_TRUE(
            obdz::ZFileCompress(src, plain_dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1)).ok());
    }

    std::shared_ptr<MemoryFile> overwritten_dst(new MemoryFile);
    {
        std::shared_ptr<MemoryFile> src(new MemoryFile);
        src->Fill(data);
        obdz::CompressArgs args = ArgsFor(obdz::CompressOptions::kLz4, 0, 1);
        args.overwrite_header = true;
        MT_EXPECT_TRUE(obdz::ZFileCompress(src, overwritten_dst, args).ok());
    }

    const core::Expected<obdz::HeaderTrailer, std::string> plain =
        obdz::HeaderTrailer::DecodeFrom512(&plain_dst->Bytes()[0], obdz::kHeaderTrailerSpace);
    const core::Expected<obdz::HeaderTrailer, std::string> rewritten =
        obdz::HeaderTrailer::DecodeFrom512(&overwritten_dst->Bytes()[0],
                                           obdz::kHeaderTrailerSpace);
    MT_EXPECT_TRUE(plain.ok() && rewritten.ok());
    if (!plain.ok() || !rewritten.ok()) return;

    MT_EXPECT_TRUE(!plain.value().IsHeaderOverwrite());
    MT_EXPECT_TRUE(rewritten.value().IsHeaderOverwrite());
    // Both are still headers of a data file, and both still validate: the
    // rewrite recomputes the digest rather than leaving the old one in place.
    MT_EXPECT_TRUE(rewritten.value().IsHeader());
    MT_EXPECT_TRUE(rewritten.value().IsDataFile());
    MT_EXPECT_TRUE(rewritten.value().IsValid(&overwritten_dst->Bytes()[0],
                                             obdz::kHeaderTrailerSpace));
    // The rewritten header carries the index the trailer points at, which the
    // placeholder cannot.
    MT_EXPECT_TRUE(rewritten.value().index_offset > 0);
    MT_EXPECT_EQ(static_cast<long long>(plain.value().index_offset), 0LL);
}

// ============================================================
// ZFileBuilder
// ============================================================

MT_TEST(builder_round_trips_unaligned_chunks) {
    // Rust `test_verify_builder`. The chunk sizes are deliberately coprime
    // with the 4 KiB block size, so most writes leave a partial tail in
    // `reserved_buf` and the completion path runs on nearly every call.
    const std::vector<uint8_t> data = SampleData(33333);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 1));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;

    size_t off = 0;
    const size_t steps[5] = {1, 777, 4095, 4097, 10000};
    int step = 0;
    while (off < data.size()) {
        const size_t want = steps[step % 5];
        const size_t n = (data.size() - off) < want ? (data.size() - off) : want;
        const core::Expected<size_t, std::string> w = builder.value().Write(&data[off], n);
        MT_EXPECT_TRUE(w.ok());
        if (!w.ok()) return;
        MT_EXPECT_EQ(static_cast<long long>(w.value()), static_cast<long long>(n));
        off += n;
        ++step;
    }
    MT_EXPECT_TRUE(builder.value().Finish().ok());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) MT_EXPECT_TRUE(back.value() == data);
}

MT_TEST(a_tail_shorter_than_one_block_is_only_flushed_by_finish) {
    // The short tail lives in the builder, not the file, until `Finish`. A
    // builder that wrote it eagerly would emit a sub-block-sized block in the
    // middle of the stream, which the jump table cannot describe.
    const std::vector<uint8_t> data = SampleData(4096 + 100);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;

    MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
    const size_t before_finish = dst->Bytes().size();
    MT_EXPECT_TRUE(builder.value().Finish().ok());
    MT_EXPECT_TRUE(dst->Bytes().size() > before_finish);

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) {
        // The whole 4196 bytes, not a block-rounded 4096 or 8192.
        MT_EXPECT_EQ(static_cast<long long>(back.value().size()),
                     static_cast<long long>(data.size()));
        MT_EXPECT_TRUE(back.value() == data);
    }
}

MT_TEST(a_builder_that_was_never_finished_leaves_an_unreadable_file) {
    // Stated as an expectation rather than a wart: without the index and
    // trailer the file is not a readable zfile, and the reader says so. The
    // alternative -- accepting it -- would decode to a silently truncated
    // image.
    const std::vector<uint8_t> data = SampleData(20000);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    {
        core::Expected<obdz::ZFileBuilder, std::string> builder =
            obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
        MT_EXPECT_TRUE(builder.ok());
        if (!builder.ok()) return;
        MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
        // Dropped without Finish.
    }
    MT_EXPECT_TRUE(!obdz::ZFileOpenRo(dst, false).ok());
}

MT_TEST(finish_is_idempotent_and_close_returns_the_destination) {
    const std::vector<uint8_t> data = SampleData(5000);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;
    MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());

    MT_EXPECT_TRUE(builder.value().Finish().ok());
    const size_t after_first = dst->Bytes().size();
    // A second Finish must not append another index and trailer.
    MT_EXPECT_TRUE(builder.value().Finish().ok());
    MT_EXPECT_EQ(static_cast<long long>(dst->Bytes().size()),
                 static_cast<long long>(after_first));

    const core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> closed =
        builder.value().Close();
    MT_EXPECT_TRUE(closed.ok());
    if (closed.ok()) MT_EXPECT_TRUE(closed.value().get() == dst.get());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) MT_EXPECT_TRUE(back.value() == data);
}

MT_TEST(writing_after_finish_is_refused) {
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;
    MT_EXPECT_TRUE(builder.value().Finish().ok());

    const uint8_t byte = 7;
    const core::Expected<size_t, std::string> w = builder.value().Write(&byte, 1);
    MT_EXPECT_TRUE(!w.ok());
    if (!w.ok()) MT_EXPECT_TRUE(Contains(w.error(), "already closed"));
}

MT_TEST(a_moved_from_builder_refuses_work_instead_of_dereferencing_nothing) {
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    core::Expected<obdz::ZFileBuilder, std::string> created =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(created.ok());
    if (!created.ok()) return;

    obdz::ZFileBuilder moved_to(created.take_value());
    // `created`'s payload was moved out; the shell must be inert, not fatal.
    obdz::ZFileBuilder& emptied = created.value();
    const uint8_t byte = 1;
    const core::Expected<size_t, std::string> w = emptied.Write(&byte, 1);
    MT_EXPECT_TRUE(!w.ok());
    if (!w.ok()) MT_EXPECT_TRUE(Contains(w.error(), "moved from"));
    MT_EXPECT_TRUE(!emptied.Finish().ok());

    // The destination moved with it and still works.
    MT_EXPECT_TRUE(moved_to.Write(&byte, 1).ok());
    MT_EXPECT_TRUE(moved_to.Finish().ok());
}

// ============================================================
// the worker pool
// ============================================================

MT_TEST(the_worker_fan_out_produces_byte_identical_output) {
    // The whole point of the pool is that it is an optimisation. Any
    // difference in the emitted bytes would be a correctness bug, and one
    // that only appears on multi-worker configurations -- so single-worker
    // output is the reference.
    //
    // 512 KiB is deliberate: `WriteBatchSize(4096)` is exactly that, so one
    // Write produces a 128-block batch, which is the only shape that fans
    // out at all.
    const std::vector<uint8_t> data = SampleData(512 * 1024);

    std::vector<uint8_t> single;
    {
        std::shared_ptr<MemoryFile> dst(new MemoryFile);
        core::Expected<obdz::ZFileBuilder, std::string> builder =
            obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 1));
        MT_EXPECT_TRUE(builder.ok());
        if (!builder.ok()) return;
        MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
        MT_EXPECT_TRUE(builder.value().Finish().ok());
        single = dst->Bytes();
    }

    const size_t worker_counts[3] = {2, 4, 7};
    for (int i = 0; i < 3; ++i) {
        std::shared_ptr<MemoryFile> dst(new MemoryFile);
        core::Expected<obdz::ZFileBuilder, std::string> builder = obdz::ZFileBuilder::Create(
            dst, ArgsFor(obdz::CompressOptions::kLz4, 1, worker_counts[i]));
        MT_EXPECT_TRUE(builder.ok());
        if (!builder.ok()) continue;
        MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
        MT_EXPECT_TRUE(builder.value().Finish().ok());

        // Byte-for-byte, not merely "also readable": the segments must be
        // committed in submission order, and an ordering bug would still
        // produce a readable file whose blocks are permuted.
        MT_EXPECT_TRUE(dst->Bytes() == single);

        const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
        MT_EXPECT_TRUE(back.ok());
        if (back.ok()) MT_EXPECT_TRUE(back.value() == data);
    }
}

MT_TEST(a_failing_worker_returns_an_error_instead_of_hanging) {
    // Rust `compact_writer_worker_panic_returns_error_instead_of_hanging`.
    // The pool's contract is one result per submitted item; a worker that
    // failed without answering would leave the collection loop waiting for a
    // result that can never arrive. This test completing at all is the
    // assertion -- a regression here hangs rather than fails.
    const std::vector<uint8_t> data = SampleData(512 * 1024);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 4));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;

    builder.value().InjectWorkerFailureForTest();
    const core::Expected<size_t, std::string> w = builder.value().Write(&data[0], data.size());
    MT_EXPECT_TRUE(!w.ok());
    if (!w.ok()) MT_EXPECT_TRUE(Contains(w.error(), "injected zfile worker failure"));
}

MT_TEST(a_pool_failure_falls_back_to_the_inline_compressor) {
    // After a pool error the pool is dropped, so the *next* write must still
    // succeed through the in-line compressor rather than failing forever or
    // reusing a half-drained queue. The file that results has to be readable,
    // which is what rules out "it continued but wrote garbage".
    const std::vector<uint8_t> first = SampleData(512 * 1024);
    const std::vector<uint8_t> second = SampleData(8192);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    core::Expected<obdz::ZFileBuilder, std::string> builder =
        obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 4));
    MT_EXPECT_TRUE(builder.ok());
    if (!builder.ok()) return;

    builder.value().InjectWorkerFailureForTest();
    MT_EXPECT_TRUE(!builder.value().Write(&first[0], first.size()).ok());

    // The injection flag lives on the pool, which has now been dropped, so
    // this write takes the in-line path and succeeds.
    MT_EXPECT_TRUE(builder.value().Write(&second[0], second.size()).ok());
    MT_EXPECT_TRUE(builder.value().Finish().ok());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) MT_EXPECT_TRUE(back.value() == second);
}

// ============================================================
// the sync-pwrite fast path
// ============================================================

MT_TEST(the_local_file_fast_path_writes_the_same_bytes_as_the_generic_path) {
    // `CommitCompressedBatch` picks a synchronous pwrite for large batches
    // bound for a `LocalFile`. `WriteCalls()` cannot tell the two apart (both
    // increment it), so the assertion is the one that actually matters: the
    // two paths must agree byte for byte. `MemoryFile` is not a `LocalFile`,
    // so it is guaranteed to take the generic route.
    const std::vector<uint8_t> data = SampleData(512 * 1024);

    std::vector<uint8_t> generic;
    {
        std::shared_ptr<MemoryFile> dst(new MemoryFile);
        core::Expected<obdz::ZFileBuilder, std::string> builder =
            obdz::ZFileBuilder::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 1, 1));
        MT_EXPECT_TRUE(builder.ok());
        if (!builder.ok()) return;
        MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
        MT_EXPECT_TRUE(builder.value().Finish().ok());
        generic = dst->Bytes();
    }
    // The batch has to exceed the 4096-byte threshold for the fast path to be
    // eligible at all, or this test would compare two generic writes.
    MT_EXPECT_TRUE(generic.size() > 4096);

    const std::string path = TempPath("fastpath");
    std::shared_ptr<obd::LocalFile> local = OpenLocal(path);
    MT_EXPECT_TRUE(static_cast<bool>(local));
    if (!local) return;

    {
        core::Expected<obdz::ZFileBuilder, std::string> builder =
            obdz::ZFileBuilder::Create(local, ArgsFor(obdz::CompressOptions::kLz4, 1, 1));
        MT_EXPECT_TRUE(builder.ok());
        if (!builder.ok()) return;
        MT_EXPECT_TRUE(builder.value().Write(&data[0], data.size()).ok());
        MT_EXPECT_TRUE(builder.value().Finish().ok());
    }

    const core::Expected<uint64_t, std::string> size = local->Size();
    MT_EXPECT_TRUE(size.ok());
    if (size.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(size.value()),
                     static_cast<long long>(generic.size()));
    }
    std::vector<uint8_t> on_disk(generic.size(), 0);
    const core::Expected<size_t, std::string> read =
        local->ReadAtInto(0, &on_disk[0], on_disk.size());
    MT_EXPECT_TRUE(read.ok());
    MT_EXPECT_TRUE(on_disk == generic);

    ::unlink(path.c_str());
}

// ============================================================
// ZFileCompactWriter
// ============================================================

MT_TEST(the_compact_writer_round_trips_through_its_buffer_interface) {
    const std::vector<uint8_t> data = SampleData(700000);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kZstd, 1, 2));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    MT_EXPECT_TRUE(writer.value()->RequiresOrderedWrites());
    MT_EXPECT_EQ(static_cast<long long>(writer.value()->BufferSize()),
                 static_cast<long long>(obdz::kZFileCompactWriterBufferSize));

    uint64_t offset = 0;
    size_t done = 0;
    while (done < data.size()) {
        core::Expected<agentenv::storage::util::CompactBuffer, std::string> buf =
            writer.value()->AllocBuffer();
        MT_EXPECT_TRUE(buf.ok());
        if (!buf.ok()) return;
        const size_t remain = data.size() - done;
        const size_t n = remain < buf.value().size() ? remain : buf.value().size();
        std::memcpy(&buf.value()[0], &data[done], n);
        MT_EXPECT_TRUE(writer.value()->Write(buf.value(), offset, n).ok());
        offset += n;
        done += n;
    }
    MT_EXPECT_TRUE(writer.value()->Finalize().ok());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) MT_EXPECT_TRUE(back.value() == data);
}

MT_TEST(the_compact_writer_write_all_at_chunks_and_round_trips) {
    // `WriteAllAt` must split anything larger than one buffer, advancing the
    // expected offset per chunk. A version that passed the whole slice to one
    // builder write would still produce a readable file, so the length check
    // below is what distinguishes them.
    const std::vector<uint8_t> data = SampleData(3 * obdz::kZFileCompactWriterBufferSize + 1234);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    MT_EXPECT_TRUE(writer.value()->WriteAllAt(&data[0], data.size(), 0).ok());
    MT_EXPECT_TRUE(writer.value()->Finalize().ok());

    const core::Expected<std::vector<uint8_t>, std::string> back = ReadBackAll(dst);
    MT_EXPECT_TRUE(back.ok());
    if (back.ok()) {
        MT_EXPECT_EQ(static_cast<long long>(back.value().size()),
                     static_cast<long long>(data.size()));
        MT_EXPECT_TRUE(back.value() == data);
    }
}

MT_TEST(the_compact_writer_refuses_an_out_of_order_offset) {
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    const std::vector<uint8_t> data = SampleData(8192);
    MT_EXPECT_TRUE(writer.value()->WriteAllAt(&data[0], 4096, 0).ok());

    // A gap, which a stream compressor cannot honour: the bytes before it
    // have already been encoded.
    const core::Expected<core::Unit, std::string> skipped =
        writer.value()->WriteAllAt(&data[4096], 4096, 8192);
    MT_EXPECT_TRUE(!skipped.ok());
    if (!skipped.ok()) {
        MT_EXPECT_TRUE(Contains(skipped.error(), "received offset 8192, expected 4096"));
    }
}

MT_TEST(the_compact_writer_checks_the_offset_of_an_empty_write_all_at) {
    // Rust `compact_writer_validates_empty_write_all_at`. An empty write is
    // not a no-op: it still asserts where the writer is. Returning early for
    // `len == 0` would accept a caller that had lost track of the offset and
    // only fail on its next real write, by which time the offset it reported
    // is gone.
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    // At the right offset an empty write is accepted...
    MT_EXPECT_TRUE(writer.value()->WriteAllAt(NULL, 0, 0).ok());
    // ...and at the wrong one it is refused.
    const core::Expected<core::Unit, std::string> bad = writer.value()->WriteAllAt(NULL, 0, 4096);
    MT_EXPECT_TRUE(!bad.ok());
    if (!bad.ok()) MT_EXPECT_TRUE(Contains(bad.error(), "received offset 4096, expected 0"));
}

MT_TEST(the_compact_writer_rejects_a_length_longer_than_its_buffer) {
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    agentenv::storage::util::CompactBuffer small(16, 0);
    const core::Expected<core::Unit, std::string> over = writer.value()->Write(small, 0, 4096);
    MT_EXPECT_TRUE(!over.ok());
    if (!over.ok()) MT_EXPECT_TRUE(Contains(over.error(), "exceeds buffer"));
}

MT_TEST(the_compact_writer_is_poisoned_after_a_write_failure) {
    // Rust `compact_writer_poisoned_after_data_write_failure`. Continuing
    // after a failed data write would produce a file whose block index does
    // not describe its data -- accepted by the format, unreadable in fact.
    const std::vector<uint8_t> data = SampleData(64 * 1024);
    std::shared_ptr<MemoryFile> dst(new MemoryFile);

    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    // The header write has already happened; fail everything from here.
    dst->FailAfterWrites(0);

    const core::Expected<core::Unit, std::string> failed =
        writer.value()->WriteAllAt(&data[0], data.size(), 0);
    MT_EXPECT_TRUE(!failed.ok());

    // Even with writes working again, the writer stays refused.
    dst->FailAfterWrites(-1);
    const core::Expected<core::Unit, std::string> after =
        writer.value()->WriteAllAt(&data[0], data.size(), 0);
    MT_EXPECT_TRUE(!after.ok());
    if (!after.ok()) MT_EXPECT_TRUE(Contains(after.error(), "failed after previous write error"));

    // ...and so does Finalize, rather than sealing a file it knows is broken.
    const core::Expected<core::Unit, std::string> finalized = writer.value()->Finalize();
    MT_EXPECT_TRUE(!finalized.ok());
    if (!finalized.ok()) {
        MT_EXPECT_TRUE(Contains(finalized.error(), "failed after previous write error"));
    }
}

MT_TEST(the_compact_writer_refuses_work_after_finalize) {
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    const core::Expected<std::shared_ptr<obdz::ZFileCompactWriter>, std::string> writer =
        obdz::ZFileCompactWriter::Create(dst, ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(writer.ok());
    if (!writer.ok()) return;

    const std::vector<uint8_t> data = SampleData(4096);
    MT_EXPECT_TRUE(writer.value()->WriteAllAt(&data[0], data.size(), 0).ok());
    MT_EXPECT_TRUE(writer.value()->Finalize().ok());
    // Idempotent, like the builder's Finish.
    MT_EXPECT_TRUE(writer.value()->Finalize().ok());

    const core::Expected<core::Unit, std::string> late =
        writer.value()->WriteAllAt(&data[0], data.size(), 4096);
    MT_EXPECT_TRUE(!late.ok());
    if (!late.ok()) MT_EXPECT_TRUE(Contains(late.error(), "already finalized"));
}

// ============================================================
// compress_batch
// ============================================================

MT_TEST(compress_batch_refuses_a_destination_stride_below_max_dst_size) {
    // The stride is how the caller locates each encoded block afterwards, so
    // a short one would let block N's output run into block N+1's slot. The
    // check has to be against `MaxDstSize()`, not against the input size:
    // every *compressible* block would fit a smaller slot, and only an
    // incompressible one would overflow it.
    const core::Expected<std::shared_ptr<obdz::Compressor>, std::string> compressor =
        obdz::CreateCompressor(ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(compressor.ok());
    if (!compressor.ok()) return;

    const std::vector<uint8_t> src = SampleData(8192);
    std::vector<size_t> src_chunk_len;
    src_chunk_len.push_back(4096);
    src_chunk_len.push_back(4096);
    std::vector<size_t> dst_chunk_len(2, 0);

    // Two slots of exactly 4096, which is less than LZ4's bound for a 4096
    // block.
    std::vector<uint8_t> too_small(2 * 4096, 0);
    const core::Expected<core::Unit, std::string> refused = compressor.value()->CompressBatch(
        &src[0], src.size(), src_chunk_len, &too_small[0], too_small.size(), &dst_chunk_len, 2);
    MT_EXPECT_TRUE(!refused.ok());
    if (!refused.ok()) MT_EXPECT_TRUE(Contains(refused.error(), "smaller than max_dst_size"));

    // With a large enough stride it succeeds, and the two blocks decode back
    // to the input -- which is what shows the refusal above was about the
    // stride and not about the call being wrong in some other way.
    const size_t stride = compressor.value()->MaxDstSize();
    std::vector<uint8_t> big(2 * stride, 0);
    const core::Expected<core::Unit, std::string> done = compressor.value()->CompressBatch(
        &src[0], src.size(), src_chunk_len, &big[0], big.size(), &dst_chunk_len, 2);
    MT_EXPECT_TRUE(done.ok());
    if (!done.ok()) return;

    for (size_t i = 0; i < 2; ++i) {
        std::vector<uint8_t> block(4096, 0);
        const core::Expected<size_t, std::string> n = compressor.value()->Decompress(
            &big[i * stride], dst_chunk_len[i], &block[0], block.size());
        MT_EXPECT_TRUE(n.ok());
        if (!n.ok()) continue;
        MT_EXPECT_EQ(static_cast<long long>(n.value()), 4096LL);
        MT_EXPECT_TRUE(std::memcmp(&block[0], &src[i * 4096], 4096) == 0);
    }
}

MT_TEST(compress_batch_with_zero_blocks_is_a_success_and_touches_nothing) {
    const core::Expected<std::shared_ptr<obdz::Compressor>, std::string> compressor =
        obdz::CreateCompressor(ArgsFor(obdz::CompressOptions::kLz4, 0, 1));
    MT_EXPECT_TRUE(compressor.ok());
    if (!compressor.ok()) return;

    std::vector<size_t> none;
    std::vector<size_t> out;
    const core::Expected<core::Unit, std::string> done =
        compressor.value()->CompressBatch(NULL, 0, none, NULL, 0, &out, 0);
    MT_EXPECT_TRUE(done.ok());
}

MT_TEST(an_unknown_algorithm_is_refused_rather_than_defaulted) {
    obdz::CompressOptions opt;
    opt.algo = 99;
    const core::Expected<std::shared_ptr<obdz::Compressor>, std::string> refused =
        obdz::CreateCompressor(obdz::CompressArgs(opt));
    MT_EXPECT_TRUE(!refused.ok());

    // And a builder over it fails at Create rather than part-way through a
    // stream -- but note the header has already been written by then, which
    // is upstream's order too.
    std::shared_ptr<MemoryFile> dst(new MemoryFile);
    MT_EXPECT_TRUE(!obdz::ZFileBuilder::Create(dst, obdz::CompressArgs(opt)).ok());
}

MT_TEST(a_zero_block_size_is_refused) {
    obdz::CompressOptions opt;
    opt.block_size = 0;
    const core::Expected<std::shared_ptr<obdz::Compressor>, std::string> refused =
        obdz::CreateCompressor(obdz::CompressArgs(opt));
    MT_EXPECT_TRUE(!refused.ok());
    if (!refused.ok()) MT_EXPECT_TRUE(Contains(refused.error(), "block_size must be > 0"));
}

MT_MAIN
