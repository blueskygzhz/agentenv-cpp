// SPDX-License-Identifier: MIT
// Rust: storage/util/src/compact_writer.rs — CompactBuffer / CompactWriter.
#ifndef AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_
#define AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace util {

/// Rust trait `CompactBuffer` — a writable byte buffer handed out by a writer.
///
/// Rust declares it as `AsMut<[u8]> + AsRef<[u8]> + Any + Send` with a blanket
/// impl, so a plain `Vec<u8>` already satisfies it; the trait exists so a
/// shared-memory implementation can hand back a buffer inside a pre-registered
/// region instead. The alias is the same bargain in C++: the default writer
/// returns plain vectors, and a pooled writer would subclass the owner rather
/// than the buffer.
typedef std::vector<uint8_t> CompactBuffer;

/// Little-endian append helper over a `CompactBuffer`.
///
/// NOT a port of Rust's `CompactWriter`, despite what this file used to claim.
/// Rust's `CompactWriter` is the buffer-allocating positional file writer
/// below; this is a serialisation helper with no Rust counterpart, and the two
/// were sharing a name. Renamed rather than deleted because the behaviour is
/// tested, but note that `lsmt.cc` carries its own private copy of these four
/// methods and does not use this one.
class LeBufferWriter {
 public:
    explicit LeBufferWriter(CompactBuffer* buf) : buf_(buf) {}

    void PutU8(uint8_t v);
    void PutU32(uint32_t v);
    void PutU64(uint64_t v);
    void PutBytes(const uint8_t* p, size_t n);

 size_t Position() const { return buf_->size(); }

 private:
 CompactBuffer* buf_;
};

/// Rust trait `CompactWriter` — where the compact/commit path sends its output.
///
/// Two phases, so that a shared-memory destination can avoid a copy: ask for a
/// buffer, fill it, then hand it back with the offset to write it at. A writer
/// backed by registered memory returns buffers inside that region, so the
/// "fill" step writes straight into the pages the device will read.
class CompactWriter {
 public:
    virtual ~CompactWriter() {}

    /// Rust `alloc_buffer` — a buffer of at least `BufferSize()` bytes.
    virtual core::Expected<CompactBuffer, std::string> AllocBuffer() = 0;

    /// Rust `buffer_size`. Should be a multiple of the 512-byte alignment:
    /// the destination may be opened `O_DIRECT`, which rejects anything else.
    virtual size_t BufferSize() const = 0;

    /// Rust `requires_ordered_writes` — whether each write must start where
    /// the previous one ended.
    ///
    /// Default false, permitting random writes. A sequential-only destination
    /// (a pipe, a streaming upload) returns true, and the caller has to order
    /// its writes rather than discover the constraint on the second one.
    virtual bool RequiresOrderedWrites() const { return false; }

    /// Rust `write` — write the first `len` bytes of `buf` at `offset`.
    ///
    /// `len` may be shorter than the buffer, which is how a partially filled
    /// buffer is submitted. Longer is an error, not a clamp: a caller asking
    /// to write more than it filled has miscounted, and clamping would commit
    /// whatever the tail of the buffer happened to hold.
    virtual core::Expected<core::Unit, std::string> Write(const CompactBuffer& buf,
                                                           uint64_t offset,
                                                           size_t len) = 0;

    /// Rust `write_all_at` — write a caller-owned slice, for small metadata.
    ///
    /// The default chunks through `AllocBuffer`, which is why an
    /// implementation that can write a borrowed slice directly overrides it.
    virtual core::Expected<core::Unit, std::string> WriteAllAt(const uint8_t* data,
                                                                size_t len,
                                                                uint64_t offset);

    /// Rust `finalize` — complete format-level output.
    ///
    /// Explicitly *not* a flush or a sync: the default is a no-op, and a
    /// caller that needs durability asks the underlying file for it.
    virtual core::Expected<core::Unit, std::string> Finalize() {
        return core::Unit();
    }
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_
