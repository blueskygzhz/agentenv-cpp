// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/bk_download.rs — the background
// download scheduler.
//
// It exists to turn an on-demand cache into a warm one: after an image opens,
// the blocks the guest has not touched yet are fetched in the background so a
// later fault finds them already there. Everything here is therefore
// best-effort -- a failure never blocks an image open, because the foreground
// read path refills on demand regardless.
//
// Porting note on concurrency. Rust runs on tokio: `JoinSet`, `Semaphore`,
// `Notify`, and cancellation by dropping a future. This port is synchronous,
// so each of those becomes an explicit thread, a counting semaphore, or a
// condition variable. The places where that changes observable behaviour are
// marked `Deviation:` at the declaration concerned.
#ifndef AGENTENV_STORAGE_OVERLAYBD_BK_DOWNLOAD_H_
#define AGENTENV_STORAGE_OVERLAYBD_BK_DOWNLOAD_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/cache_pool.h"
#include "agentenv/storage/overlaybd/config.h"
#include "agentenv/storage/overlaybd/startup_pack_task.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `POLL_INTERVAL` — how often an interruptible wait rechecks `running`.
const uint64_t kBkPollIntervalMs = 200;

/// Rust `DEFAULT_HEDGE_TIMEOUT` — per-chunk read budget.
const uint64_t kBkDefaultHedgeTimeoutMs = 6000;

/// Rust `HEDGE_MAX_ATTEMPTS`.
const uint32_t kBkHedgeMaxAttempts = 3;

/// Rust `PENDING_WAIT_FALLBACK` — cap on the pending-import park.
const uint64_t kBkPendingWaitFallbackMs = 5000;

/// Rust `BkDownloadSubmitError::Closed`'s message and `category()`.
///
/// A fixed category, never the source URL or credentials: these strings reach
/// logs, and a cache source name is a signed URL.
extern const char kBkSubmitClosedMessage[];
extern const char kBkSubmitClosedCategory[];

/// Rust `download_error_category`.
///
/// Deviation: Rust walks the error chain and downcasts to `JoinError` /
/// `io::Error` to classify. Errors here are strings, so this matches on their
/// text instead. The category set is identical, but a message that does not
/// mention its cause lands in the generic bucket where Rust would have
/// recognised the type.
///
/// The match is also locale-dependent, which has been measured rather than
/// assumed: these strings carry `strerror`, and under `LANG=zh_CN.UTF-8`
/// `strerror(ENOENT)` is "没有那个文件或目录", so an English-only probe never
/// fires. That is tolerable **here and only here** because the result is a
/// log label and nothing branches on it — a mislabelled failure is still
/// logged, retried and reported identically. Where such a test would decide
/// control flow it must not be used; `image_file.cc` had exactly that bug and
/// now probes the path instead (see `LocalLayerIsMissing`).
const char* DownloadErrorCategory(const std::string& error);

/// Rust `background_download_delay` — `delay` plus up to `delay_extra`
/// seconds of jitter.
///
/// The jitter is the point: without it every layer of an image would start
/// downloading in the same second, and the burst would compete with the very
/// foreground reads the delay exists to stay out of.
uint64_t BackgroundDownloadDelayMs(const DownloadConfig& config);

/// Rust `retry_backoff` — `2^(attempt-1)` seconds, capped at 30, plus up to
/// 1s derived from `seed`.
///
/// The jitter is seeded by the cache id rather than drawn randomly so two
/// layers that fail together do not retry in lockstep, while one layer's
/// schedule stays reproducible.
uint64_t RetryBackoffMs(uint32_t attempt, const std::string& seed);

/// Rust `tokio::sync::Semaphore`, reduced to what the scheduler uses.
///
/// `Close` is the part that matters: Rust closes the semaphores on shutdown so
/// that a task already blocked on a permit wakes and fails, rather than
/// waiting for a permit nobody will release. A plain counting semaphore would
/// deadlock the drain.
class BkSemaphore {
 public:
    explicit BkSemaphore(size_t permits) : permits_(permits), closed_(false) {}

    /// Blocks for a permit. False once closed — including for a caller that
    /// was already waiting when the close happened.
    bool Acquire();
    void Release();
    void Close();
    bool IsClosed() const;

 private:
    BkSemaphore(const BkSemaphore&);
    BkSemaphore& operator=(const BkSemaphore&);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    size_t permits_;
    bool closed_;
};

/// RAII permit, so a run releases its slot on every exit path.
class BkPermit {
 public:
    BkPermit(BkSemaphore* sem, bool held) : sem_(sem), held_(held) {}
    ~BkPermit() {
        if (held_ && sem_ != 0) sem_->Release();
    }
    bool held() const { return held_; }
    /// Hand the permit to someone else; this object stops owning it.
    void Disarm() { held_ = false; }

 private:
    BkPermit(const BkPermit&);
    BkPermit& operator=(const BkPermit&);
    BkSemaphore* sem_;
    bool held_;
};

/// Rust `BkDownloadTask` — one registered download of a blob into the cache.
///
/// Holds no cache entry or open handle while pending, so the entry stays
/// evictable until the task actually runs; the run phase reopens it.
struct BkDownloadTask {
    std::string cache_id;
    /// The source name to reopen by. `cache_id` is a digest of it, so it
    /// cannot be recovered from the id.
    std::string cache_key;
    std::shared_ptr<VirtualFile> source;
    uint64_t source_size;
    DownloadConfig config;
    /// Background fetch granularity in *cache* blocks: one source request
    /// covers this many. The cache block size itself is untouched, so
    /// foreground reads keep their fine granularity while background reads
    /// keep large-request throughput.
    uint32_t blocks_per_chunk;
    core::Optional<std::string> device_key;
    /// Not owned. The backend creates and outlives the scheduler, exactly as
    /// Rust's `_bk_scheduler_owner` field arranges.
    FileCacheBackend* backend;

    BkDownloadTask() : source_size(0), blocks_per_chunk(1), backend(0) {}
};

/// Rust `FairReadyQueue<T>`, specialised to the one type that uses it.
///
/// Ready tasks are grouped by device key and drained round-robin, so one image
/// with fifty layers cannot starve a later image of download turns.
class FairReadyQueue {
 public:
    void Push(const core::Optional<std::string>& device_key,
              const std::shared_ptr<BkDownloadTask>& item);

    /// Takes the front group's front item, then rotates that group to the
    /// back when it still has work.
    ///
    /// False when there is nothing to take. Matching Rust, an *empty* front
    /// group also yields false and is dropped rather than skipped over; groups
    /// are never left empty, so this does not arise.
    bool Pop(std::shared_ptr<BkDownloadTask>* out);

    void Clear();
    size_t Size() const;
    /// Drops every queued entry equal to `task`; returns how many went.
    size_t RemoveAll(const std::shared_ptr<BkDownloadTask>& task);

 private:
    struct Group {
        core::Optional<std::string> device_key;
        std::deque<std::shared_ptr<BkDownloadTask> > items;
    };
    std::deque<Group> groups_;
};

/// One submission: the cached file to warm, its policy, and the sandbox device
/// whose readiness gates it.
struct BkDownloadRequest {
    std::shared_ptr<CachedFile> file;
    DownloadConfig config;
    /// Absent for a consumer that is not a sandbox, which then skips the
    /// readiness wait entirely.
    core::Optional<std::string> device_key;
};

class BkDownloadScheduler;

/// Rust `StartupPackHandle` — one holder of a registered startup prefetch.
///
/// Scheduler-backed rather than task-backed: the handle's last release has to
/// pull the task out of the registry *and* out of the ready queue, which only
/// the scheduler can do.
class StartupPackHandle {
 public:
    StartupPackHandle() : scheduler_(0) {}
    StartupPackHandle(const std::shared_ptr<BkDownloadScheduler>& scheduler,
                      const std::string& task_key);
    StartupPackHandle(const StartupPackHandle& other);
    StartupPackHandle& operator=(const StartupPackHandle& other);
    ~StartupPackHandle();

    /// `kStartupPackFailed` when the task is no longer registered, matching
    /// Rust's `unwrap_or(Failed)`: a handle to something the scheduler has
    /// forgotten cannot report progress.
    StartupPackPhase Phase() const;
    /// Absent when the task is gone.
    core::Optional<StartupPackStatsSnapshot> Stats() const;
    const std::string& TaskKey() const { return task_key_; }

    /// Blocks until the task is terminal. Rust marks the equivalent
    /// `#[cfg(test)]`; resume never waits on a prefetch.
    StartupPackPhase WaitTerminal();

 private:
    std::shared_ptr<StartupPackTask> Task() const;

    std::shared_ptr<BkDownloadScheduler> scheduler_;
    std::string task_key_;
};

/// Rust `BkDownloadScheduler`.
///
/// Shared-from-this because a `StartupPackHandle` keeps the scheduler alive:
/// the handle's destructor touches the registry, so the scheduler cannot be
/// allowed to outlive its handles the other way round.
class BkDownloadScheduler : public std::enable_shared_from_this<BkDownloadScheduler> {
 public:
    /// Rust `new`.
    ///
    /// Both caps come from the *global* download config at backend creation.
    /// A per-image `maxInflightBlocks` override never resizes them, so the
    /// scratch budget `max_inflight_blocks * chunk_size` holds for every task.
    static std::shared_ptr<BkDownloadScheduler> New(size_t max_inflight_blocks,
                                                       size_t max_concurrent_files,
                                                       uint32_t chunk_size,
                                                       uint64_t hedge_timeout_ms);

    ~BkDownloadScheduler();

    /// Rust `submit` — register background downloads.
    ///
    /// Never fails for lack of capacity: tasks wait in the registry until a
    /// slot frees. Only a shut-down scheduler is reported; a request with an
    /// unusable source or an already-complete cache is skipped with a logged
    /// category, because failing the batch would block an image open over a
    /// warm-up that is optional by design.
    core::Expected<core::Unit, std::string> Submit(
        FileCacheBackend* backend, const std::vector<BkDownloadRequest>& requests);

    /// Rust `submit_startup_pack`.
    ///
    /// A concurrent submission of the same immutable pack identity attaches to
    /// the live task instead of duplicating the download.
    core::Expected<StartupPackHandle, std::string> SubmitStartupPack(
        const StartupPackSubmission& submission);

    /// Rust `shutdown` — stop accepting work and wait for every owned thread
    /// to leave the running state. Idempotent.
    void Shutdown();

    /// Rust `stop` — close submission and signal every worker to wind down.
    ///
    /// Closing the semaphores is persistent, unlike a notification pulse, so a
    /// thread already blocked on a permit is guaranteed to wake.
    void Stop();

    const std::atomic<bool>* Running() const { return &running_; }
    BkSemaphore* BlockSlots() { return block_slots_.get(); }
    uint32_t MaxInflightBlocks() const { return static_cast<uint32_t>(max_inflight_blocks_); }

    /// Rust `startup_waiting` — true while a startup pack is queued for
    /// dispatch. Running background tasks check this at chunk boundaries and
    /// yield their file slot.
    bool StartupWaiting() const { return startup_waiting_.load() > 0; }

    bool IsRegistered(const std::string& cache_id) const;
    size_t RegisteredCount() const;
    /// Startup packs still in the registry. Rust marks the background
    /// equivalents `#[cfg(test)]`; this one exists for the same reason --
    /// deregistration is a property worth asserting, not just reviewing.
    size_t StartupRegisteredCount() const;
    bool IsClosed() const;

 private:
    friend class StartupPackHandle;

    BkDownloadScheduler();
    BkDownloadScheduler(const BkDownloadScheduler&);
    BkDownloadScheduler& operator=(const BkDownloadScheduler&);

    /// How a background task's run ended. Rust `RunOutcome`.
    enum RunOutcome {
        /// Complete or terminally failed: deregister.
        kRunDone,
        /// Only pending-import holes remain: park, then requeue, keeping the
        /// registration.
        ///
        /// Deviation in reachability, not in code: Rust's
        /// `actionable_background_chunks` filters nothing, so an empty chunk
        /// list always means a complete bitmap and this outcome never occurs.
        /// Ported because the state machine has it and a future pending filter
        /// would need it.
        kRunWaitPending,
        /// A startup pack is queued: give up the file slot and requeue.
        kRunYieldStartup
    };

    /// Rust `ChunksOutcome`.
    enum ChunksOutcome { kChunksDone, kChunksYieldStartup, kChunksFailed };

    struct Job {
        bool is_startup;
        std::shared_ptr<BkDownloadTask> task;
        std::shared_ptr<StartupPackTask> startup;
        bool holds_file;
        bool holds_startup;

        Job() : is_startup(false), holds_file(false), holds_startup(false) {}
    };

    /// A timer or requeue thread, plus the flag it sets on exit so finished
    /// ones can be joined without waiting on live ones.
    struct AuxThread {
        std::shared_ptr<std::thread> thread;
        std::shared_ptr<std::atomic<bool> > done;
    };

    void DispatchLoop();
    void WorkerLoop();

    /// Rust `wait_then_mark_ready` — the timer phase, holding no execution
    /// resource: readiness and the configured delay are waited out before the
    /// task competes for a slot.
    void WaitThenMarkReady(const std::shared_ptr<BkDownloadTask>& task);

    RunOutcome RunTask(const std::shared_ptr<BkDownloadTask>& task);
    ChunksOutcome DownloadChunks(const std::shared_ptr<BkDownloadTask>& task,
                                 const std::shared_ptr<CachedFile>& file,
                                 const std::vector<std::pair<uint64_t, uint32_t> >& chunks,
                                 struct BkThrottle* throttle, std::string* first_error);

    /// Rust `refill_chunk_with_hedge`.
    core::Expected<uint64_t, std::string> RefillChunkWithHedge(
        const std::shared_ptr<CachedFile>& file, const std::shared_ptr<VirtualFile>& source,
        uint64_t source_size, uint64_t start_block, uint32_t len_blocks);

    void ThrottleAfterBlock(struct BkThrottle* throttle, uint64_t bytes);
    void InterruptibleSleep(uint64_t duration_ms);

    /// Rust's `remove_if(.., Arc::ptr_eq)` — only deregister this very task,
    /// so a newer registration for the same cache id survives.
    void RemoveTask(const std::shared_ptr<BkDownloadTask>& task);

    void PushReady(const std::shared_ptr<BkDownloadTask>& task);
    void EnqueueJob(const Job& job);
    void SpawnAux(const std::shared_ptr<BkDownloadTask>& task, bool timer_phase,
                  bool wait_pending);
    void ReapAux();
    void JoinAux();

    void WarnOverrideMismatchOnce(size_t requested);
    void WarnChunkClampOnce(uint64_t requested);

    void ActiveInc();
    void ActiveDec();

    size_t max_inflight_blocks_;
    uint32_t chunk_size_;
    uint64_t hedge_timeout_ms_;

    std::atomic<bool> running_;

    mutable std::mutex closed_mu_;
    bool closed_;

    mutable std::mutex tasks_mu_;
    std::map<std::string, std::shared_ptr<BkDownloadTask> > tasks_;

    mutable std::mutex ready_mu_;
    std::condition_variable ready_cv_;
    FairReadyQueue ready_;

    mutable std::mutex startup_mu_;
    std::map<std::string, std::shared_ptr<StartupPackTask> > startup_tasks_;
    std::deque<std::shared_ptr<StartupPackTask> > startup_ready_;
    /// Counted rather than derived from the queue: a task already popped by
    /// the dispatcher but still waiting for a slot must keep backgrounds
    /// yielding.
    std::atomic<size_t> startup_waiting_;

    std::unique_ptr<BkSemaphore> file_slots_;
    std::unique_ptr<BkSemaphore> block_slots_;
    std::unique_ptr<BkSemaphore> startup_slots_;

    mutable std::mutex jobs_mu_;
    std::condition_variable jobs_cv_;
    std::deque<Job> jobs_;

    /// Live scheduler-owned threads; `Shutdown` waits for this to reach zero.
    std::atomic<size_t> active_;
    mutable std::mutex idle_mu_;
    std::condition_variable idle_cv_;

    /// Fires whenever a pending-import set changes, releasing parked tasks.
    mutable std::mutex pending_mu_;
    std::condition_variable pending_cv_;

    std::thread dispatcher_;
    std::vector<std::thread> workers_;

    mutable std::mutex aux_mu_;
    std::vector<AuxThread> aux_;

    std::atomic<bool> override_mismatch_warned_;
    std::atomic<bool> chunk_clamp_warned_;
};

// ==== backend-facing submission ==========================================
//
// Rust has these three as methods on `FileCacheBackend`. They are free
// functions here purely because their signatures name types declared in this
// header, which includes `cache_pool.h` -- putting them on the class would
// close the include cycle. `backend` is the receiver Rust's `&self` is, and
// the behaviour is the same, including resolving the scheduler through the
// backend rather than taking one as an argument.

/// Rust `FileCacheBackend::submit_bk_download_batch`.
///
/// Submission never fails for lack of capacity: tasks wait in the registry
/// until a file slot frees. A shut-down scheduler *is* reported, which is the
/// case `image_service` downgrades to a warning so a warm-up that is optional
/// by design cannot fail an image open.
core::Expected<core::Unit, std::string> SubmitBkDownloadBatch(
    FileCacheBackend* backend, const std::vector<BkDownloadRequest>& requests);

/// Rust `FileCacheBackend::submit_bk_download` — the one-request form.
core::Expected<core::Unit, std::string> SubmitBkDownload(
    FileCacheBackend* backend, std::shared_ptr<CachedFile> file,
    const DownloadConfig& config, const core::Optional<std::string>& device_key);

/// Rust `FileCacheBackend::submit_startup_pack`.
///
/// A concurrent submission of the same immutable pack identity shares one
/// task; the returned handle is a holder, and the last release of a live task
/// cancels it.
core::Expected<StartupPackHandle, std::string> SubmitStartupPack(
    FileCacheBackend* backend, const StartupPackSubmission& submission);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_BK_DOWNLOAD_H_
