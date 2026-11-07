// SPDX-License-Identifier: MIT
// Rust: no direct counterpart — see the header.
#include "agentenv/storage/overlaybd/compact_writer_sink.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

bool CompactWriterSink::WriteAt(uint64_t off, const uint8_t* data, uint64_t len) {
    if (!writer_) {
        if (!failed_) {
            failed_ = true;
            error_ = "compact writer sink has no writer";
        }
        return false;
    }
    // Once a write has failed the stream is incomplete, so a later one cannot
    // land where it claims to. Refused here rather than forwarded, both to
    // keep the first (specific) error instead of overwriting it with a
    // follow-on one, and because a sequential writer would reject the offset
    // anyway with a message about the gap rather than about the real cause.
    if (failed_) return false;

    // A zero-length write at a given offset is not a request to do anything,
    // and for a sequential writer it must not advance the expected offset
    // either. `CompactTo` already guards its data and index writes, but the
    // trailer and header paths do not, so this is the one place that has to
    // be total.
    if (len == 0) return true;

    // `len` is a `uint64_t` in this interface and a `size_t` in the writer's.
    // They are the same width on every target this builds for, but narrowing
    // without a check is how a 32-bit build would silently write a truncated
    // tail, so it is rejected rather than cast.
    const size_t want = static_cast<size_t>(len);
    if (static_cast<uint64_t>(want) != len) {
        failed_ = true;
        error_ = "compact write length exceeds the addressable range";
        return false;
    }

    const core::Expected<core::Unit, std::string> put = writer_->WriteAllAt(data, want, off);
    if (!put.ok()) {
        failed_ = true;
        error_ = put.error();
        return false;
    }
    return true;
}

core::Expected<core::Unit, std::string> CompactWriterSink::Finalize() {
    if (!writer_) {
        return core::make_unexpected(std::string("compact writer sink has no writer"));
    }
    if (failed_) {
        // The write error is what a caller needs to see, not a second-order
        // complaint from the finalizer about the stream it was handed.
        return core::make_unexpected(
            std::string("refusing to finalize after a failed write: ") + error_);
    }
    return writer_->Finalize();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
