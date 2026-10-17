// SPDX-License-Identifier: MIT
// Rust: storage/util/src/compact_writer.rs
#include "agentenv/storage/util/compact_writer.h"

#include <cstring>

namespace agentenv {
namespace storage {
namespace util {

void LeBufferWriter::PutU8(uint8_t v) {
    buf_->push_back(v);
}
void LeBufferWriter::PutU32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void LeBufferWriter::PutU64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void LeBufferWriter::PutBytes(const uint8_t* p, size_t n) {
    buf_->insert(buf_->end(), p, p + n);
}

core::Expected<core::Unit, std::string> CompactWriter::WriteAllAt(const uint8_t* data,
                                                                   size_t len,
                                                                   uint64_t offset) {
    if (len == 0) return core::Unit();
    if (data == NULL) {
        return core::make_unexpected(std::string("CompactWriter: null data"));
    }
    const size_t chunk_size = BufferSize();
    if (chunk_size == 0) {
        return core::make_unexpected(std::string("CompactWriter: zero buffer size"));
    }

    size_t done = 0;
    uint64_t at = offset;
    while (done < len) {
        const size_t take = (len - done) < chunk_size ? (len - done) : chunk_size;
        core::Expected<CompactBuffer, std::string> buf = AllocBuffer();
        if (!buf.ok()) return core::make_unexpected(buf.error());

        CompactBuffer owned = buf.take_value();
        // The writer promised at least `BufferSize()`; a shorter buffer would
        // silently truncate this chunk, so it is refused rather than trimmed.
        if (owned.size() < take) {
            return core::make_unexpected(
                std::string("CompactWriter: allocated buffer smaller than its own "
                            "declared buffer size"));
        }
        std::memcpy(&owned[0], data + done, take);

        const core::Expected<core::Unit, std::string> w = Write(owned, at, take);
        if (!w.ok()) return core::make_unexpected(w.error());

        done += take;
        at += take;
    }
    return core::Unit();
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
