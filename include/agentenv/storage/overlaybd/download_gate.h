// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/download_gate.rs — admission control between
// foreground faults and background downloads.
//
// The problem it solves: a background download and a guest page fault compete
// for the same remote bandwidth, and the fault is the one a user is waiting
// on. So background block reads are parked while any foreground read is in
// flight -- but never to zero, because a download that never progresses never
// finishes.
#ifndef AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_
#define AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `BK_FLOOR_INFLIGHT` — background reads always allowed, even when
/// foreground traffic is saturated.
///
/// One, not zero: a trickle keeps downloads completing instead of starving
/// them outright for as long as the guest is faulting.
const int64_t kBkFloorInflight = 1;

/// Rust `STARTUP_FLOOR_INFLIGHT`.
///
/// Much higher than the steady-state floor because startup prefetch fetches
/// the *same* working set the guest is about to fault on -- it is as
/// latency-critical as the fault path, only more efficient. Still bounded, so
/// a resume storm cannot drown demand reads.
const int64_t kStartupFloorInflight = 8;

/// Rust `GATE_BACKOFF` — re-check interval while parked.
const int64_t kGateBackoffMs = 200;

/// Rust `SANDBOX_READY_FALLBACK`.
///
/// If no readiness signal arrives within this window -- signal lost, sandbox
/// failed, or the consumer is not a sandbox at all -- the download starts
/// anyway. The wait is a courtesy, never a deadlock.
const int64_t kSandboxReadyFallbackSec = 20;

/// Rust `FgReadGuard` — one foreground remote read in flight.
///
/// Counted at the image-file read entry. Background downloads bypass that
/// path entirely (they read their own source handle), so they never create
/// one of these -- which is what makes the count mean "a guest is waiting".
class FgReadGuard {
 public:
    FgReadGuard();
    ~FgReadGuard();

 private:
    FgReadGuard(const FgReadGuard&);
    FgReadGuard& operator=(const FgReadGuard&);
};

/// Rust `BkReadPermit` — an admitted background read.
///
/// Holds the background count elevated for the duration of the actual remote
/// read, so the next admission check sees it. Only obtainable from
/// `GateBlockRead`; the increment happens there, on admission.
class BkReadPermit {
 public:
    ~BkReadPermit();

 private:
    friend class DownloadGate;
    BkReadPermit() {}
    BkReadPermit(const BkReadPermit&);
    BkReadPermit& operator=(const BkReadPermit&);
};

class DownloadGate {
 public:
    /// Rust `notify_sandbox_ready` — release downloads held for this device.
    ///
    /// Called when the owning process reports the sandbox finished booting.
    static void NotifySandboxReady(const std::string& device_key);

    /// Rust `wait_sandbox_ready`.
    ///
    /// Returns when the device is reported ready, the fallback elapses, or
    /// `running` goes false -- whichever comes first. All three are normal
    /// returns: the caller proceeds either way, and distinguishing them would
    /// imply a failure mode that does not exist here.
    static void WaitSandboxReady(const std::string& device_key, int64_t fallback_sec,
                                 const std::atomic<bool>* running);

    /// Rust `gate_block_read` — park until a background read is admitted.
    ///
    /// Null when `running` went false while parked, so a cancelled download
    /// never delays the drain path.
    static std::shared_ptr<BkReadPermit> GateBlockRead(const std::atomic<bool>* running);

    /// Rust `gate_startup_block_read` — the same with the startup floor.
    static std::shared_ptr<BkReadPermit> GateStartupBlockRead(
        const std::atomic<bool>* running);

    /// Rust `gate_block_read_with_floor`.
    static std::shared_ptr<BkReadPermit> GateBlockReadWithFloor(
        const std::atomic<bool>* running, int64_t floor);

    /// Rust `try_admit_block_read` — one admission check, no parking.
    ///
    /// Exposed because a caller driving its own loop (and the tests) needs the
    /// check without the 200ms sleep baked in.
    static std::shared_ptr<BkReadPermit> TryAdmitBlockRead(const std::atomic<bool>* running,
                                                              int64_t floor);

    static int64_t ForegroundInflight();
    static int64_t BackgroundInflight();

    /// Clears the ready registry. Tests only: the registry is process-global,
    /// so one test's readiness would otherwise satisfy another's wait.
    static void ResetReadyRegistryForTesting();
};

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_DOWNLOAD_GATE_H_
