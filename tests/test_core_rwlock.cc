// SPDX-License-Identifier: MIT
// Tests for `core::RwLock` — the C++11 stand-in for Rust's `RwLock`.
#include "microtest.h"

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "agentenv/core/rwlock.h"

using namespace agentenv;

namespace {

/// Shared counter plus the peak it ever reached, so "how many holders were
/// inside at once" is an observable rather than a timing guess.
struct Occupancy {
    pthread_mutex_t mu;
    size_t live;
    size_t peak;

    Occupancy() : live(0), peak(0) { ::pthread_mutex_init(&mu, 0); }
    ~Occupancy() { ::pthread_mutex_destroy(&mu); }

    void Enter() {
        ::pthread_mutex_lock(&mu);
        ++live;
        if (live > peak) peak = live;
        ::pthread_mutex_unlock(&mu);
    }
    void Leave() {
        ::pthread_mutex_lock(&mu);
        --live;
        ::pthread_mutex_unlock(&mu);
    }
    size_t Live() {
        ::pthread_mutex_lock(&mu);
        const size_t v = live;
        ::pthread_mutex_unlock(&mu);
        return v;
    }
    size_t Peak() {
        ::pthread_mutex_lock(&mu);
        const size_t v = peak;
        ::pthread_mutex_unlock(&mu);
        return v;
    }
};

const size_t kThreads = 4;

/// How long each holder stays inside. Long enough that creating the four
/// threads is negligible beside it, so a shared lock reliably has all four in
/// flight at once; short enough that the exclusive run, which serialises into
/// `kThreads * kHoldMs`, stays quick.
const uint64_t kHoldMs = 200;

struct WorkerArgs {
    core::RwLock* lock;
    Occupancy* occupancy;
    bool shared;
};

/// Enters the lock, stays a fixed while, leaves.
///
/// No barrier: an earlier version had the threads wait for each other, which
/// is unimplementable here -- the first thread to see the full count leaves
/// and drops it again, so the rest wait forever. A fixed hold makes the peak
/// occupancy the whole observation, and it needs no coordination.
void* Worker(void* raw) {
    WorkerArgs* args = static_cast<WorkerArgs*>(raw);
    if (args->shared) {
        core::ReadGuard guard(*args->lock);
        args->occupancy->Enter();
        ::usleep(static_cast<useconds_t>(kHoldMs * 1000));
        args->occupancy->Leave();
    } else {
        core::WriteGuard guard(*args->lock);
        args->occupancy->Enter();
        ::usleep(static_cast<useconds_t>(kHoldMs * 1000));
        args->occupancy->Leave();
    }
    return 0;
}

void RunWorkers(core::RwLock* lock, Occupancy* occupancy, bool shared) {
    std::vector<pthread_t> threads(kThreads);
    std::vector<WorkerArgs> args(kThreads);
    for (size_t i = 0; i < kThreads; ++i) {
        args[i].lock = lock;
        args[i].occupancy = occupancy;
        args[i].shared = shared;
        if (::pthread_create(&threads[i], 0, Worker, &args[i]) != 0) {
            std::fprintf(stderr, "  FAIL: pthread_create\n");
            std::exit(1);
        }
    }
    for (size_t i = 0; i < kThreads; ++i) ::pthread_join(threads[i], 0);
}

}  // namespace

MT_TEST(readers_hold_the_lock_at_the_same_time) {
    core::RwLock lock;
    Occupancy occupancy;
    RunWorkers(&lock, &occupancy, true);

    // All four were inside at once, which an exclusive lock makes impossible.
    MT_EXPECT_EQ(static_cast<long long>(occupancy.Peak()), static_cast<long long>(kThreads));
    MT_EXPECT_EQ(static_cast<long long>(occupancy.Live()), static_cast<long long>(0));
}

MT_TEST(writers_exclude_each_other) {
    core::RwLock lock;
    Occupancy occupancy;
    RunWorkers(&lock, &occupancy, false);

    // The point of the exclusive guard: never two inside at once. Same
    // workload as the test above, so the two differ only in the guard.
    MT_EXPECT_EQ(static_cast<long long>(occupancy.Peak()), static_cast<long long>(1));
    MT_EXPECT_EQ(static_cast<long long>(occupancy.Live()), static_cast<long long>(0));
}

MT_TEST(the_lock_is_released_when_its_guard_dies) {
    // Both guards are RAII, so a second acquisition after the first scope
    // ends must not block. Without release this test hangs rather than
    // failing, which the harness timeout surfaces.
    core::RwLock lock;
    { core::ReadGuard guard(lock); }
    { core::WriteGuard guard(lock); }
    { core::ReadGuard guard(lock); }
    { core::WriteGuard guard(lock); }
    MT_EXPECT_TRUE(true);
}

MT_MAIN
