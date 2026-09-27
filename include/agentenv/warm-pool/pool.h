// SPDX-License-Identifier: MIT
// Rust: crates/warm-pool/  — generic warm resource pool with watermark-based
// maintenance.
//
// This C++11 port is a strict alignment with the upstream Rust `WarmPool<T>`:
// - watermark-based refill/drain decisions (`compute_maintenance_action`)
// - geometric growth of the refill target under acquisition pressure
// - shutdown coordination with safe resource cleanup (`drain_all`)
//
// The Rust maintenance worker requires `&'static self` (it hands the pool to a
// background thread for the process lifetime). That ownership model does not
// translate cleanly to a value-type C++ class, so the background worker is
// exposed via `RequestMaintenance` + an externally-driven `run_cycle` rather
// than an owned thread. All pool *mechanics* (the semantic core) are aligned
// one-to-one with the Rust implementation.
#ifndef AGENTENV_WARM_POOL_H_
#define AGENTENV_WARM_POOL_H_

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace agentenv {
namespace warmpool {

/// Action computed by watermark logic for the maintenance worker.
/// Mirrors Rust `PoolMaintenanceAction`.
enum class PoolMaintenanceKind { Fill, Drain, Idle };

struct PoolMaintenanceAction {
    PoolMaintenanceKind kind;
    std::size_t count;  // meaningful for Fill / Drain; 0 for Idle

    static PoolMaintenanceAction Fill(std::size_t n) {
        return PoolMaintenanceAction{PoolMaintenanceKind::Fill, n};
    }
    static PoolMaintenanceAction Drain(std::size_t n) {
        return PoolMaintenanceAction{PoolMaintenanceKind::Drain, n};
    }
    static PoolMaintenanceAction Idle() {
        return PoolMaintenanceAction{PoolMaintenanceKind::Idle, 0};
    }

    bool operator==(const PoolMaintenanceAction& o) const {
        return kind == o.kind && count == o.count;
    }
    bool operator!=(const PoolMaintenanceAction& o) const { return !(*this == o); }
};

/// Configuration for a warm pool. Mirrors Rust `PoolConfig`.
struct PoolConfig {
    std::size_t low_watermark = 0;
    std::size_t high_watermark = 0;
    bool maintenance_enabled = false;
    /// Advisory flag for callers that can prewarm once a reusable resource
    /// shape is known; `WarmPool` itself only owns generic pool mechanics.
    bool startup_prewarm = false;

    /// Validate and normalize config values (mirrors Rust `PoolConfig::validate`).
    PoolConfig Validate() const {
        PoolConfig out = *this;
        if (out.low_watermark > out.high_watermark) {
            // low_watermark > high_watermark; clamp low to high.
            out.low_watermark = out.high_watermark;
        }
        return out;
    }
};

/// Generic warm pool for reusable resources. `T` is the pooled resource type.
///
/// With maintenance enabled, `high_watermark` is a drain target rather than a
/// strict insertion cap: release paths may temporarily exceed it, and the
/// maintenance driver is expected to drain excess resources.
template <typename T>
class WarmPool {
 public:
    explicit WarmPool(PoolConfig config)
        : config_(config.Validate()),
          fill_target_(std::min(config_.low_watermark, config_.high_watermark)),
          pending_(false),
          stop_(false),
          shutting_down_(false) {}

    /// Check if the pool is shutting down.
    bool IsShuttingDown() const {
        std::lock_guard<std::mutex> lg(mu_);
        return shutting_down_;
    }

    /// Return the validated pool configuration.
    const PoolConfig& Config() const { return config_; }

    /// Return the current number of idle resources.
    std::size_t Len() const {
        std::lock_guard<std::mutex> lg(mu_);
        return pool_.size();
    }

    /// Return whether the pool currently has no idle resources.
    bool IsEmpty() const { return Len() == 0; }

    /// Compute the maintenance action based on current pool size.
    PoolMaintenanceAction ComputeMaintenanceAction(std::size_t pool_len) const {
        std::lock_guard<std::mutex> lg(mu_);
        return ComputeMaintenanceActionLocked(pool_len);
    }

    /// Try to acquire a resource from the pool (fast path).
    /// Returns whether a resource was produced into `*out`.
    bool TryAcquire(T* out) {
        std::size_t next_pool_len;
        {
            std::lock_guard<std::mutex> lg(mu_);
            if (shutting_down_) {
                return false;
            }
            if (pool_.empty()) {
                next_pool_len = 0;
                RecordAcquisitionPressureLocked(next_pool_len);
                return false;
            }
            *out = std::move(pool_.front());
            pool_.pop_front();
            next_pool_len = pool_.size();
            RecordAcquisitionPressureLocked(next_pool_len);
        }
        return true;
    }

    /// Try to acquire the first resource matching `predicate`.
    template <typename Predicate>
    bool TryAcquireWhere(Predicate predicate, T* out) {
        std::lock_guard<std::mutex> lg(mu_);
        if (shutting_down_) {
            return false;
        }
        bool found = false;
        for (auto it = pool_.begin(); it != pool_.end(); ++it) {
            if (predicate(*it)) {
                *out = std::move(*it);
                pool_.erase(it);
                found = true;
                break;
            }
        }
        RecordAcquisitionPressureLocked(pool_.size());
        return found;
    }

    /// Try to enqueue an idle resource only if the pool is below the high
    /// watermark. Returns true on success; on failure `*resource` is untouched
    /// (still owned by the caller), mirroring Rust's `Result<(), T>`.
    bool TryPushBounded(T resource) {
        std::lock_guard<std::mutex> lg(mu_);
        if (!shutting_down_ && pool_.size() < config_.high_watermark) {
            pool_.push_back(std::move(resource));
            return true;
        }
        return false;
    }

    /// Drain one idle resource from the back of the pool.
    bool TryDrainOne(T* out) {
        std::lock_guard<std::mutex> lg(mu_);
        if (pool_.empty()) {
            return false;
        }
        *out = std::move(pool_.back());
        pool_.pop_back();
        return true;
    }

    /// Return a resource to the pool.
    ///
    /// If maintenance is enabled, enqueues even when above the high watermark
    /// so the maintenance driver owns all drain decisions. If disabled,
    /// respects the high watermark and returns false (resource is dropped by
    /// value / left to the caller) when the pool is full.
    ///
    /// Returns true when the resource was enqueued.
    bool Release(T resource) {
        bool should_request = false;
        {
            std::lock_guard<std::mutex> lg(mu_);
            if (!shutting_down_ &&
                (config_.maintenance_enabled || pool_.size() < config_.high_watermark)) {
                std::size_t next_pool_len = pool_.size() + 1;
                pool_.push_back(std::move(resource));
                if (next_pool_len < config_.low_watermark ||
                    next_pool_len > config_.high_watermark) {
                    should_request = true;
                }
            } else {
                return false;
            }
        }
        if (should_request) {
            RequestMaintenance();
        }
        return true;
    }

    /// Drain all resources from the pool and return them. Intended for shutdown
    /// cleanup; after calling this the pool rejects new releases.
    std::vector<T> DrainAll() {
        {
            std::lock_guard<std::mutex> lg(mu_);
            shutting_down_ = true;
        }
        StopMaintenance();
        std::vector<T> out;
        std::lock_guard<std::mutex> lg(mu_);
        while (!pool_.empty()) {
            out.push_back(std::move(pool_.front()));
            pool_.pop_front();
        }
        return out;
    }

    /// Request the maintenance driver to wake up and check watermarks.
    /// Mirrors Rust `request_maintenance` (sets the pending flag + notifies).
    void RequestMaintenance() {
        std::lock_guard<std::mutex> lg(mu_);
        if (!config_.maintenance_enabled || shutting_down_ || stop_) {
            return;
        }
        pending_ = true;
        cv_.notify_one();
    }

    /// Block until maintenance work is pending or the pool stops. Returns true
    /// if there is work to do (the caller should run one maintenance cycle),
    /// false if the driver should exit.
    bool WaitForMaintenance() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || pending_; });
        if (stop_) {
            return false;
        }
        pending_ = false;
        return true;
    }

    /// Signal any maintenance driver to stop and wake it up.
    void StopMaintenance() {
        std::lock_guard<std::mutex> lg(mu_);
        stop_ = true;
        pending_ = true;
        cv_.notify_all();
    }

 private:
    PoolMaintenanceAction ComputeMaintenanceActionLocked(std::size_t pool_len) const {
        std::size_t fill_target = CurrentFillTargetLocked();
        if (pool_len < fill_target) {
            std::size_t to_fill = fill_target - pool_len;  // pool_len < fill_target
            if (to_fill > 0) {
                return PoolMaintenanceAction::Fill(to_fill);
            }
        }
        if (pool_len > config_.high_watermark) {
            std::size_t to_drain = pool_len - config_.high_watermark;
            if (to_drain > 0) {
                return PoolMaintenanceAction::Drain(to_drain);
            }
        }
        return PoolMaintenanceAction::Idle();
    }

    std::size_t CurrentFillTargetLocked() const {
        return std::min(fill_target_, config_.high_watermark);
    }

    void GrowFillTargetAfterPressureLocked(std::size_t pool_len) {
        if (pool_len >= config_.low_watermark || config_.high_watermark == 0) {
            return;
        }
        std::size_t low = std::min(config_.low_watermark, config_.high_watermark);
        std::size_t base = std::max<std::size_t>(std::max(fill_target_, low), 1);
        std::size_t next = SaturatingMul2(base);
        next = std::min(next, config_.high_watermark);
        fill_target_ = next;
    }

    void RecordAcquisitionPressureLocked(std::size_t pool_len) {
        GrowFillTargetAfterPressureLocked(pool_len);
        PoolMaintenanceAction action = ComputeMaintenanceActionLocked(pool_len);
        if (action.kind == PoolMaintenanceKind::Fill &&
            config_.maintenance_enabled && !shutting_down_ && !stop_) {
            pending_ = true;
            cv_.notify_one();
        }
    }

    static std::size_t SaturatingMul2(std::size_t v) {
        std::size_t max = static_cast<std::size_t>(-1);
        if (v > max / 2) {
            return max;
        }
        return v * 2;
    }

    PoolConfig config_;
    std::deque<T> pool_;
    std::size_t fill_target_;
    bool pending_;
    bool stop_;
    bool shutting_down_;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
};

}  // namespace warmpool
}  // namespace agentenv
#endif  // AGENTENV_WARM_POOL_H_
