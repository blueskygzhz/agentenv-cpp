// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/download_gate.rs
#include "agentenv/storage/overlaybd/download_gate.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

/// Rust `FG_REMOTE_INFLIGHT` / `BK_REMOTE_INFLIGHT`.
///
/// Process-global, matching Rust's `static AtomicI64`. The gate is a property
/// of the host's link, not of any one image, so per-instance counters would
/// let N images each run a floor's worth of background traffic.
std::atomic<int64_t>& FgInflight() {
    static std::atomic<int64_t> v(0);
    return v;
}

std::atomic<int64_t>& BkInflight() {
    static std::atomic<int64_t> v(0);
    return v;
}

/// Rust `ReadyRegistry`.
///
/// Rust pairs a mutex-guarded set with a `Notify`; here the notify is a
/// condition variable on the same mutex. The set is the state and the signal
/// is only a wakeup -- a waiter that misses the notification still sees the
/// set on its next poll, which is why the wait is bounded rather than purely
/// event-driven.
struct ReadyRegistry {
    std::mutex mu;
    std::condition_variable cv;
    std::set<std::string> ready;
};

ReadyRegistry& Registry() {
    static ReadyRegistry r;
    return r;
}

}  // namespace

FgReadGuard::FgReadGuard() { FgInflight().fetch_add(1); }
FgReadGuard::~FgReadGuard() { FgInflight().fetch_sub(1); }

BkReadPermit::~BkReadPermit() { BkInflight().fetch_sub(1); }

void DownloadGate::NotifySandboxReady(const std::string& device_key) {
    ReadyRegistry& r = Registry();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.ready.insert(device_key);
    }
    // Every waiter, not one: several downloads may be held on the same device
    // key, and readiness releases all of them.
    r.cv.notify_all();
}

void DownloadGate::WaitSandboxReady(const std::string& device_key, int64_t fallback_sec,
                                    const std::atomic<bool>* running) {
    ReadyRegistry& r = Registry();
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(fallback_sec);

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(r.mu);
            if (r.ready.count(device_key) != 0) return;

            // Checked inside the loop rather than once up front: a download
            // can be cancelled while it is parked here, and the drain path
            // must not wait out the full fallback.
            if (running != 0 && !running->load()) return;

            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            if (now >= deadline) return;

            // Capped at the backoff even when the deadline is far away, so
            // `running` is re-checked at that interval. The condition variable
            // would otherwise sleep until a notification that may never come.
            std::chrono::milliseconds slice(kGateBackoffMs);
            const std::chrono::milliseconds left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (left < slice) slice = left;
            r.cv.wait_for(lk, slice);
        }
    }
}

std::shared_ptr<BkReadPermit> DownloadGate::TryAdmitBlockRead(
    const std::atomic<bool>* running, int64_t floor) {
    if (running != 0 && !running->load()) return std::shared_ptr<BkReadPermit>();

    // The floor only applies under foreground pressure. With no foreground
    // read in flight there is nothing to protect, so background reads are
    // admitted without bound -- that is where a download actually makes
    // progress.
    const bool foreground_busy = FgInflight().load() > 0;
    if (!foreground_busy) {
        BkInflight().fetch_add(1);
        return std::shared_ptr<BkReadPermit>(new BkReadPermit());
    }

    // Rust's `try_update`: increment only while below the floor. A plain
    // load-then-add would let several threads each see `floor - 1` and all
    // admit, so the bound has to be established by the exchange itself.
    int64_t current = BkInflight().load();
    for (;;) {
        if (current >= floor) return std::shared_ptr<BkReadPermit>();
        if (BkInflight().compare_exchange_weak(current, current + 1)) {
            return std::shared_ptr<BkReadPermit>(new BkReadPermit());
        }
        // `current` was refreshed by the failed exchange; retry against it.
    }
}

std::shared_ptr<BkReadPermit> DownloadGate::GateBlockReadWithFloor(
    const std::atomic<bool>* running, int64_t floor) {
    for (;;) {
        const std::shared_ptr<BkReadPermit> permit = TryAdmitBlockRead(running, floor);
        if (permit) return permit;
        if (running != 0 && !running->load()) return std::shared_ptr<BkReadPermit>();

        // Polled rather than woken. Rust does the same: the thing being
        // waited on is a *count reaching zero*, and the foreground path must
        // not pay for signalling background waiters on every read completion.
        std::this_thread::sleep_for(std::chrono::milliseconds(kGateBackoffMs));
    }
}

std::shared_ptr<BkReadPermit> DownloadGate::GateBlockRead(const std::atomic<bool>* running) {
    return GateBlockReadWithFloor(running, kBkFloorInflight);
}

std::shared_ptr<BkReadPermit> DownloadGate::GateStartupBlockRead(
    const std::atomic<bool>* running) {
    return GateBlockReadWithFloor(running, kStartupFloorInflight);
}

int64_t DownloadGate::ForegroundInflight() { return FgInflight().load(); }
int64_t DownloadGate::BackgroundInflight() { return BkInflight().load(); }

void DownloadGate::ResetReadyRegistryForTesting() {
    ReadyRegistry& r = Registry();
    std::lock_guard<std::mutex> lk(r.mu);
    r.ready.clear();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
