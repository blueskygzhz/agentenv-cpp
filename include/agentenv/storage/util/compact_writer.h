// SPDX-License-Identifier: MIT
// Rust: storage/util/src/compact_writer.rs — CompactWriter / CompactBuffer.
// A little-endian varint-ish append-only buffer used to serialize the index.
#ifndef AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_
#define AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace agentenv {
namespace storage {
namespace util {

/// Rust struct `CompactBuffer` — the growable backing store.
using CompactBuffer = std::vector<uint8_t>;

/// Rust struct `CompactWriter` — appends fixed-width LE integers + blobs.
class CompactWriter {
 public:
    explicit CompactWriter(CompactBuffer* buf) : buf_(buf) {}

    void PutU8(uint8_t v);
    void PutU32(uint32_t v);
    void PutU64(uint64_t v);
    void PutBytes(const uint8_t* p, size_t n);

 size_t Position() const { return buf_->size(); }

 private:
 CompactBuffer* buf_;
};

}  // namespace util
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UTIL_COMPACT_WRITER_H_
