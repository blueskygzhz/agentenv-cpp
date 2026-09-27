// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/compression/zfile.rs
#include "agentenv/storage/overlaybd/zfile.h"

#include <cstring>

namespace agentenv {
namespace storage {
namespace overlaybd {

core::Expected<int64_t, std::string>
DecodeBlock(CompressionAlgo algo, const uint8_t* src, int64_t src_len,
   uint8_t* dst, int64_t dst_cap) {
    if (algo == CompressionAlgo::None) {
     if (src_len > dst_cap) {
            return core::make_unexpected(std::string("dst too small"));
        }
    std::memcpy(dst, src, static_cast<size_t>(src_len));
        return src_len;
  }
#ifdef AGENTENV_WITH_ZSTD
    // TODO: call ZSTD_decompress / LZ4_decompress_safe.
    return core::make_unexpected(std::string("zstd/lz4 decode not yet wired"));
#else
    return core::make_unexpected(std::string("compression not compiled in (AGENTENV_WITH_ZSTD)"));
#endif
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
