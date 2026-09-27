// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/download_gate.rs — foreground/background admission gate.
#ifndef AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_
#define AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_

#include <cstdint>
#include <string>

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `BK_FLOOR_INFLIGHT`.
static const int64_t kBkFloorInflight = 1;
/// Rust `GATE_BACKOFF` (ms).
static const int64_t kGateBackoffMs = 200;
/// Rust `SANDBOX_READY_FALLBACK` (seconds).
static const int64_t kSandboxReadyFallbackSec = 20;

/// Rust process-global download gate (all counters are process-wide atomics).
class DownloadGate {
 public:
    /// Rust: enter/exit a foreground remote read.
    static void EnterForeground();
    static void ExitForeground();

    /// Rust: block until a background block read is admitted, then account it.
    /// Skeleton: non-blocking — returns immediately (respecting the floor).
    static void EnterBackground();
    static void ExitBackground();

    /// Rust: mark a sandbox device key as ready (unblocks its downloads).
    static void MarkReady(const std::string& device_key);

    static int64_t ForegroundInflight();
    static int64_t BackgroundInflight();
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_
