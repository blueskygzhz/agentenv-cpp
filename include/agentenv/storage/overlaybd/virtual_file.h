// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/io/virtual_file.rs — VirtualFile / IoCtx /
// VirtualFileWriter.
//
// The abstraction everything in the overlaybd stack is built on: Rust has 25
// implementations of it, from a local file to the merged LSMT view to the
// image file the ublk target serves. Porting it is a precondition for any of
// them.
//
// Rust's trait is async; there are no coroutines here, so these are plain
// synchronous calls. That is not a simplification of the semantics -- the
// return values and the error/no-op distinctions below are what the callers
// depend on, and they are preserved exactly.
#ifndef AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/util/compact_writer.h"
#include "agentenv/storage/util/io_ring.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `Bytes` — an owned read result.
typedef std::vector<uint8_t> ByteBuffer;

/// Rust `IoCtx` — the io_uring a particular request should be submitted on.
///
/// Threaded through the read path so a ublk queue thread issues its I/O on
/// *its own* ring rather than falling back to synchronous positional reads.
/// It has to be passed rather than looked up: each queue owns a ring created
/// `single_issuer`, so submitting on another queue's ring is undefined, not
/// merely slower.
///
/// Null is meaningful and common -- it says "no particular ring", and every
/// `*WithCtx` default falls back to the plain call.
class IoCtx {
 public:
    IoCtx() : ring_(NULL) {}
    explicit IoCtx(util::Ring* ring) : ring_(ring) {}

    util::Ring* Ring() const { return ring_; }
    bool Empty() const { return ring_ == NULL; }

 private:
    util::Ring* ring_;
};

/// Rust trait `VirtualFile`.
///
/// Three pure virtuals; everything else has a default. The defaults are not
/// uniform, and the difference between them is the contract:
///
///   * **Refusing** (`truncate`, `sync`, `discard`, the xattr setters) — an
///     implementation that cannot do these must say so. Reporting success for
///     a `sync` that did not happen, or a `truncate` that did not resize,
///     tells the caller its data is durable or its file is shorter when
///     neither is true.
///   * **Succeeding as a no-op** (`evict_range`, `evict_all`) — these are
///     hints. Doing nothing is a complete and correct implementation of a
///     hint, so failing them would make callers handle an error that carries
///     no information.
///   * **Answering "unknown"** (`seek_data`, `seek_hole`) — absent is a valid
///     answer to a query about hole layout. A file that cannot introspect its
///     own sparseness has not failed; it simply has nothing to report, and
///     the caller treats the range as data.
class VirtualFile {
 public:
    virtual ~VirtualFile() {}

    // -- required ----------------------------------------------------------

    /// Rust `read_at` — read up to `len` bytes at `offset`.
    ///
    /// A short result is success, not an error: the caller compares the
    /// returned length against what it asked for. End-of-file is the ordinary
    /// way to get one.
    virtual core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset,
                                                            size_t len) = 0;

    /// Rust `write_at` — returns the number of bytes written.
    ///
    /// Also permitted to be short, for the same reason, and for the same
    /// reason the caller must check: `VirtualFileWriter` below turns a short
    /// write into an error precisely because at the format level it is one.
    virtual core::Expected<size_t, std::string> WriteAt(uint64_t offset,
                                                         const uint8_t* data,
                                                         size_t len) = 0;

    /// Rust `size`.
    virtual core::Expected<uint64_t, std::string> Size() = 0;

    // -- provided ----------------------------------------------------------

    /// Rust `read_at_into` — read into a caller-owned buffer.
    ///
    /// Returns how many bytes landed, which may be fewer than `len`. The
    /// default goes through `ReadAt` and copies; an implementation that can
    /// read straight into the destination overrides it to skip the copy.
    virtual core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                            size_t len);

    /// Rust `read_at_with_ctx`. Default ignores `ctx` and calls `ReadAt`.
    ///
    /// Two kinds of implementation override this: one that can submit on the
    /// ring itself, and any layer that wraps another file and must pass the
    /// context down. A wrapper that forgets to forward it silently drops the
    /// whole stack below it back to synchronous I/O.
    virtual core::Expected<ByteBuffer, std::string> ReadAtWithCtx(const IoCtx& ctx,
                                                                   uint64_t offset,
                                                                   size_t len);

    /// Rust `read_at_into_with_ctx`. Routed through `ReadAtWithCtx` so an
    /// implementation only has to override that one to cover both.
    virtual core::Expected<size_t, std::string> ReadAtIntoWithCtx(const IoCtx& ctx,
                                                                   uint64_t offset,
                                                                   uint8_t* dst,
                                                                   size_t len);

    /// Rust `write_at_with_ctx`. Default ignores `ctx` and calls `WriteAt`.
    virtual core::Expected<size_t, std::string> WriteAtWithCtx(const IoCtx& ctx,
                                                                uint64_t offset,
                                                                const uint8_t* data,
                                                                size_t len);

    /// Rust `truncate`. Refused by default -- see the class comment.
    virtual core::Expected<core::Unit, std::string> Truncate(uint64_t size);

    /// Rust `sync`. Refused by default: a no-op `sync` reported as success is
    /// a durability claim this file cannot make.
    virtual core::Expected<core::Unit, std::string> Sync();

    /// Rust `seek_data` / `seek_hole` — the next data or hole at or after
    /// `offset`, or absent when this file cannot say.
    virtual core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset);
    virtual core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset);

    /// Rust `discard` — punch a hole, keeping the logical size.
    ///
    /// Refused by default. Silently ignoring it would leave the caller
    /// believing space was reclaimed.
    virtual core::Expected<core::Unit, std::string> Discard(uint64_t offset, uint64_t len);

    /// Rust `evict_range` / `evict_all` — cache hints. No-op success by
    /// default.
    virtual core::Expected<core::Unit, std::string> EvictRange(uint64_t offset,
                                                                uint64_t len);
    virtual core::Expected<core::Unit, std::string> EvictAll();

    /// Rust's xattr set, matching overlaybd's `IFileXAttr`. All refused by
    /// default: a file with no xattr support must not answer a get with
    /// emptiness, which reads as "the attribute is set and blank".
    virtual core::Expected<ByteBuffer, std::string> FGetXattr(const std::string& name);
    virtual core::Expected<std::vector<std::string>, std::string> FListXattr();
    virtual core::Expected<core::Unit, std::string> FSetXattr(const std::string& name,
                                                               const uint8_t* value,
                                                               size_t len, int flags);
    virtual core::Expected<core::Unit, std::string> FRemoveXattr(const std::string& name);

    // Rust's `as_any` has no counterpart here: it exists to let a caller
    // recover the concrete type for a fast path, which C++ does with
    // `dynamic_cast` on this interface directly.
};

/// Rust `VirtualFileWriter` — a `CompactWriter` backed by a `VirtualFile`.
class VirtualFileWriter : public util::CompactWriter {
 public:
    /// Rust `VirtualFileWriter::new` — 32 KiB buffers.
    explicit VirtualFileWriter(std::shared_ptr<VirtualFile> file);
    /// Rust `with_buffer_size`.
    VirtualFileWriter(std::shared_ptr<VirtualFile> file, size_t buf_size);

    core::Expected<util::CompactBuffer, std::string> AllocBuffer() override;
    size_t BufferSize() const override { return buf_size_; }
    core::Expected<core::Unit, std::string> Write(const util::CompactBuffer& buf,
                                                   uint64_t offset, size_t len) override;
    /// Rust's override of the default: writes the caller's bytes straight
    /// through, with no intermediate buffer.
    core::Expected<core::Unit, std::string> WriteAllAt(const uint8_t* data, size_t len,
                                                        uint64_t offset) override;

 private:
    std::shared_ptr<VirtualFile> file_;
    size_t buf_size_;
};

/// Rust's default buffer size in `VirtualFileWriter::new`.
const size_t kVirtualFileWriterBufSize = 32 * 1024;

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_VIRTUAL_FILE_H_
