// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/compression/zfile.rs — ZFile compressed blocks.
#ifndef AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust enum: the compression algorithm of a ZFile block.
enum class CompressionAlgo {
    None,
    Lz4,
    Zstd,
};

/// Rust struct `ZFileHeader`.
struct ZFileHeader {
    uint64_t    magic = 0;
    uint32_t  block_size = 0;
    CompressionAlgo algo = CompressionAlgo::None;
    uint64_t        raw_size = 0;
};

/// Rust: decode one compressed block into `dst`.
core::Expected<int64_t, std::string>
  DecodeBlock(CompressionAlgo algo,
      const uint8_t* src, int64_t src_len,
         uint8_t* dst, int64_t dst_cap);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_ZFILE_H_
