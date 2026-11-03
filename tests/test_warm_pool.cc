// SPDX-License-Identifier: MIT
// Ported from crates/warm-pool/src/lib.rs `mod tests`.
#include "agentenv/warm-pool/pool.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "microtest.h"

using agentenv::warmpool::PoolConfig;
using agentenv::warmpool::PoolMaintenanceAction;
using agentenv::warmpool::WarmPool;

namespace {
PoolConfig Cfg(std::size_t low, std::size_t high, bool maintenance) {
    PoolConfig c;
    c.low_watermark = low;
    c.high_watermark = high;
    c.maintenance_enabled = maintenance;
    c.startup_prewarm = false;
    return c;
}
}  // namespace

MT_TEST(config_validation_clamps_low_to_high) {
    PoolConfig c = Cfg(64, 32, true).Validate();
    MT_EXPECT_EQ(c.low_watermark, static_cast<std::size_t>(32));
    MT_EXPECT_EQ(c.high_watermark, static_cast<std::size_t>(32));
}

MT_TEST(compute_maintenance_action_fills_to_initial_low_watermark) {
    WarmPool<int> pool(Cfg(4, 10, true));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(2) == PoolMaintenanceAction::Fill(2));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(7) == PoolMaintenanceAction::Idle());
}

MT_TEST(acquisition_pressure_grows_fill_target_geometrically) {
    WarmPool<int> pool(Cfg(2, 10, true));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(0) == PoolMaintenanceAction::Fill(2));

    MT_EXPECT_TRUE(pool.Release(1));
    MT_EXPECT_TRUE(pool.Release(2));

    int v = 0;
    MT_EXPECT_TRUE(pool.TryAcquire(&v));
    MT_EXPECT_EQ(v, 1);
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(1) == PoolMaintenanceAction::Fill(3));

    MT_EXPECT_TRUE(pool.TryAcquire(&v));
    MT_EXPECT_EQ(v, 2);
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(0) == PoolMaintenanceAction::Fill(8));
}

MT_TEST(acquisition_misses_grow_fill_target_geometrically) {
    WarmPool<int> pool(Cfg(2, 10, false));
    int v = 0;

    MT_EXPECT_TRUE(!pool.TryAcquire(&v));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(0) == PoolMaintenanceAction::Fill(4));

    MT_EXPECT_TRUE(!pool.TryAcquire(&v));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(0) == PoolMaintenanceAction::Fill(8));

    MT_EXPECT_TRUE(!pool.TryAcquireWhere([](const int&) { return true; }, &v));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(0) == PoolMaintenanceAction::Fill(10));
}

MT_TEST(compute_maintenance_action_drains_above_high) {
    WarmPool<int> pool(Cfg(2, 4, true));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(8) == PoolMaintenanceAction::Drain(4));
    MT_EXPECT_TRUE(pool.ComputeMaintenanceAction(4) == PoolMaintenanceAction::Idle());
}

MT_TEST(try_acquire_returns_none_when_empty) {
    WarmPool<int> pool(Cfg(0, 10, false));
    int v = 0;
    MT_EXPECT_TRUE(!pool.TryAcquire(&v));
}

MT_TEST(try_acquire_returns_resource_when_available) {
    WarmPool<int> pool(Cfg(0, 10, false));
    MT_EXPECT_TRUE(pool.Release(42));
    int v = 0;
    MT_EXPECT_TRUE(pool.TryAcquire(&v));
    MT_EXPECT_EQ(v, 42);
}

MT_TEST(release_respects_high_watermark_when_maintenance_disabled) {
    WarmPool<int> pool(Cfg(0, 2, false));
    MT_EXPECT_TRUE(pool.Release(1));
    MT_EXPECT_TRUE(pool.Release(2));
    MT_EXPECT_TRUE(!pool.Release(3));
}

MT_TEST(release_allows_maintenance_worker_to_drain_above_high_watermark) {
    WarmPool<int> pool(Cfg(0, 2, true));
    MT_EXPECT_TRUE(pool.Release(1));
    MT_EXPECT_TRUE(pool.Release(2));
    MT_EXPECT_TRUE(pool.Release(3));
    MT_EXPECT_EQ(pool.Len(), static_cast<std::size_t>(3));
}

MT_TEST(drain_all_empties_pool_and_sets_shutting_down) {
    WarmPool<int> pool(Cfg(0, 10, false));
    MT_EXPECT_TRUE(pool.Release(1));
    MT_EXPECT_TRUE(pool.Release(2));
    std::vector<int> drained = pool.DrainAll();
    MT_EXPECT_EQ(drained.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(drained[0], 1);
    MT_EXPECT_EQ(drained[1], 2);
    MT_EXPECT_TRUE(pool.IsShuttingDown());
    MT_EXPECT_TRUE(pool.IsEmpty());
}

MT_TEST(try_acquire_returns_none_after_shutdown) {
    WarmPool<int> pool(Cfg(0, 10, false));
    MT_EXPECT_TRUE(pool.Release(42));
    pool.DrainAll();
    int v = 0;
    MT_EXPECT_TRUE(!pool.TryAcquire(&v));
}

MT_TEST(release_rejects_after_shutdown) {
    WarmPool<int> pool(Cfg(0, 10, false));
    pool.DrainAll();
    MT_EXPECT_TRUE(!pool.Release(42));
}

// ---- start_maintenance_worker ----

namespace {

/// Polls `done` for up to two seconds.
template <typename Pred>
bool WaitUntil(Pred done) {
    for (int i = 0; i < 400; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

}  // namespace

MT_TEST(maintenance_worker_refills_to_the_low_watermark) {
    WarmPool<int> pool(Cfg(3, 6, true));
    std::atomic<int> cycles(0);
    pool.StartMaintenanceWorker([&pool, &cycles]() {
        ++cycles;
        const PoolMaintenanceAction action = pool.ComputeMaintenanceAction(pool.Len());
        if (action.kind != agentenv::warmpool::PoolMaintenanceKind::Fill) return;
        for (std::size_t i = 0; i < action.count; ++i) pool.TryPushBounded(1);
    });
    // Started with a maintenance request, so it fills without being asked.
    MT_EXPECT_TRUE(WaitUntil([&pool]() { return pool.Len() >= 3; }));
    MT_EXPECT_EQ(pool.Len(), static_cast<std::size_t>(3));

    // An acquisition below the low watermark wakes it again.
    int out = 0;
    MT_EXPECT_TRUE(pool.TryAcquire(&out));
    pool.RequestMaintenance();
    MT_EXPECT_TRUE(WaitUntil([&pool]() { return pool.Len() >= 3; }));

    // `DrainAll` joins the worker before returning the entries.
    const std::vector<int> drained = pool.DrainAll();
    MT_EXPECT_TRUE(drained.size() >= 3);
    const int after = cycles.load();
    pool.RequestMaintenance();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MT_EXPECT_EQ(cycles.load(), after);
}

MT_TEST(maintenance_worker_is_not_started_when_disabled) {
    WarmPool<int> pool(Cfg(3, 6, false));
    std::atomic<int> cycles(0);
    pool.StartMaintenanceWorker([&cycles]() { ++cycles; });
    pool.RequestMaintenance();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MT_EXPECT_EQ(cycles.load(), 0);
}

MT_TEST(maintenance_worker_starts_once) {
    WarmPool<int> pool(Cfg(1, 2, true));
    std::atomic<int> first(0);
    std::atomic<int> second(0);
    pool.StartMaintenanceWorker([&pool, &first]() {
        ++first;
        pool.TryPushBounded(1);
    });
    pool.StartMaintenanceWorker([&second]() { ++second; });
    MT_EXPECT_TRUE(WaitUntil([&first]() { return first.load() > 0; }));
    MT_EXPECT_EQ(second.load(), 0);
    pool.DrainAll();
}

MT_MAIN
