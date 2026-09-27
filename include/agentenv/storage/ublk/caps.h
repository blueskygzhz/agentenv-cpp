// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/ublk_caps.rs — ublk capability constants.
#ifndef AGENTENV_STORAGE_UBLK_CAPS_H_
#define AGENTENV_STORAGE_UBLK_CAPS_H_

#include <cstdint>

namespace agentenv {
namespace storage {
namespace ublk {

// Rust: `struct ublksrv_ctrl_cmd` is 32 bytes in the kernel uapi.
static const uint32_t kUblksrvCtrlCmdSize = 32;

/// Rust `UBLK_U_CMD_UPDATE_SIZE` = _IOWR('u', 0x15, struct ublksrv_ctrl_cmd).
///   (3 << 30) | (sizeof << 16) | ('u' << 8) | 0x15  == 0xC020_7515
static const uint32_t kUblkUCmdUpdateSize =
    (3u << 30) | (kUblksrvCtrlCmdSize << 16) | (uint32_t('u') << 8) | 0x15u;

/// Rust `UBLK_F_UPDATE_SIZE` = 1 << 10.
static const uint64_t kUblkFUpdateSize = 1ull << 10;

/// Rust `UBLK_F_SUPPORT_ZERO_COPY` = bit 0.
static const uint64_t kUblkFSupportZeroCopy = 1ull << 0;

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UBLK_CAPS_H_
