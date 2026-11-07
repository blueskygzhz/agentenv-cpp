// SPDX-License-Identifier: MIT
// Rust: no direct counterpart — see the class comment.
#ifndef AGENTENV_STORAGE_OVERLAYBD_COMPACT_WRITER_SINK_H_
#define AGENTENV_STORAGE_OVERLAYBD_COMPACT_WRITER_SINK_H_

#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/util/compact_writer.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Bridges a `util::CompactWriter` into the `lsmt::CompactSink` that
/// `lsmt::CompactTo` writes through.
///
/// **Why this exists at all.** Rust has no counterpart because it has no
/// split: `CommitArgs.writer` is already a `dyn CompactWriter`, so
/// `compact_to` drives the trait directly. This port narrowed the compaction
/// output to `lsmt::CompactSink` (a single `bool WriteAt`) so that `lsmt.h`
/// would not have to pull in the io layer, and that narrowing is what left
/// `ZFileCompactWriter` -- a full `CompactWriter` -- unreachable from any
/// compaction. This class is that missing joint, not a second abstraction:
/// every `CompactWriter` method it does not forward is one `CompactTo` never
/// calls.
///
/// **`AllocBuffer` is deliberately not used.** The two-phase
/// `AllocBuffer`/`Write` protocol exists so a shared-memory destination can be
/// filled in place, but `CompactSink::WriteAt` hands over a caller-owned
/// pointer that is already full. Routing it through an allocated buffer would
/// add a copy to buy nothing, so this forwards to `WriteAllAt`, which is the
/// borrowed-slice entry point for exactly this case.
///
/// **Ordering.** A sequential-only writer (`RequiresOrderedWrites()`, which
/// `ZFileCompactWriter` returns true for) is safe here: this port's
/// `CompactTo` copies blocks on one thread and its offsets are contiguous
/// end to end -- header at 0, data packed immediately after, then the index,
/// its padding, and the trailer. That is Rust's `concurrency == 1` branch,
/// the one branch that needs no ordering special-case, which is why no
/// caller queries the flag. Should a concurrent `CompactTo` ever be ported,
/// Rust's middle branch (`helper.rs:1559`, concurrent reads with ordered
/// writes) is the shape to follow, and this class would be where the
/// constraint is read.
///
/// **`Finalize` is the caller's job.** `lsmt::CompactTo` returns as soon as
/// the trailer is written; it has no finalize step, because `CompactSink` has
/// no such method to call. Rust's `compact_to` ends with
/// `writer.finalize().await?` (`helper.rs:1640`), and for a zfile destination
/// that call is what emits the block index and trailer -- without it the
/// output has a header and data but no index, and every reader rejects it.
/// So a caller must invoke `Finalize()` itself after `CompactTo` succeeds.
/// It is not done in the destructor: finalizing is a write that can fail, and
/// a destructor has nowhere to report that.
class CompactWriterSink : public lsmt::CompactSink {
 public:
    explicit CompactWriterSink(std::shared_ptr<util::CompactWriter> writer)
        : writer_(writer), failed_(false) {}

    /// Forwards to `CompactWriter::WriteAllAt`.
    ///
    /// The reason a failure gives back is kept in `error()`: the `bool` here
    /// is all `CompactSink` can return, and `CompactTo` turns it into its own
    /// generic "write data failed", which would otherwise discard the only
    /// message that says *why* -- for a zfile writer typically an offset
    /// mismatch naming both offsets. `DenseDigestSink` keeps its error for
    /// the same reason.
    bool WriteAt(uint64_t off, const uint8_t* data, uint64_t len);

    /// Rust `CompactWriter::finalize`, which `CompactTo` cannot call itself.
    ///
    /// Refused after a failed write rather than attempted: the writer is
    /// already poisoned, and asking it to seal a truncated stream would
    /// either fail again with a less specific message or, worse, publish a
    /// file whose index does not describe its data.
    core::Expected<core::Unit, std::string> Finalize();

    /// Whether a write has failed, and the message it failed with. Empty
    /// while the sink is healthy.
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

 private:
    CompactWriterSink(const CompactWriterSink&);
    CompactWriterSink& operator=(const CompactWriterSink&);

    std::shared_ptr<util::CompactWriter> writer_;
    bool failed_;
    std::string error_;
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_COMPACT_WRITER_SINK_H_
