// SPDX-License-Identifier: MIT
// Rust: storage/util/src/compact_writer.rs
#include "agentenv/storage/util/compact_writer.h"

namespace agentenv {
namespace storage {
namespace util {

void CompactWriter::PutU8(uint8_t v) {
    buf_->push_back(v);
}
void CompactWriter::PutU32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void CompactWriter::PutU64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void CompactWriter::PutBytes(const uint8_t* p, size_t n) {
    buf_->insert(buf_->end(), p, p + n);
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
