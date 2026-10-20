// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/bk_download.rs
#include "agentenv/storage/overlaybd/bk_download.h"

#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/download_gate.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

const char kBkSubmitClosedMessage[] = "background download scheduler is shut down";
const char kBkSubmitClosedCategory[] = "scheduler_closed";

/// Rust `BkThrottle`.
struct BkThrottle {
    std::mutex mu;
    std::chrono::steady_clock::time_point started;
    uint64_t downloaded;
    uint64_t limit_bps;

    BkThrottle()
        : started(std::chrono::steady_clock::now()), downloaded(0), limit_bps(0) {}
};

namespace {

uint64_t NowMs() {
    struct timeval tv;
    ::gettimeofday(&tv, 0);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

bool ContainsNoCase(const std::string& hay, const char* needle) {
    const std::string n(needle);
    if (n.empty() || n.size() > hay.size()) return false;
    for (size_t i = 0; i + n.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < n.size() &&
               ::tolower(static_cast<unsigned char>(hay[i + j])) ==
                   ::tolower(static_cast<unsigned char>(n[j]))) {
            ++j;
        }
        if (j == n.size()) return true;
    }
    return false;
}

bool EqualsNoCase(const std::string& a, const char* b) {
    const std::string s(b);
    if (a.size() != s.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (::tolower(static_cast<unsigned char>(a[i])) !=
            ::tolower(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

/// Rust `warn_submit_skipped` — the fixed category and the content-addressed
/// cache id only.
///
/// Never the source name: a cache source is a signed URL, and these lines
/// reach logs that outlive the credential.
void WarnSubmitSkipped(const std::string& cache_id, const std::string& error) {
    AGENTENV_WARN("error_category=" << DownloadErrorCategory(error) << " cache_id=" << cache_id
                                    << " skipping background download submission");
}

}  // namespace

const char* DownloadErrorCategory(const std::string& error) {
    // Rust matches on the error *chain*, downcasting to `JoinError` and
    // `io::Error`. Errors here are strings, so the same categories are
    // recovered from their text. A message that does not name its cause lands
    // in the generic bucket where Rust would have recognised the type.
    if (EqualsNoCase(error, "incomplete")) return "incomplete";
    if (ContainsNoCase(error, "worker") && ContainsNoCase(error, "join")) {
        return "worker_join_error";
    }
    if (ContainsNoCase(error, "No such file") || ContainsNoCase(error, "not found")) {
        return "io_not_found";
    }
    if (ContainsNoCase(error, "Permission denied")) return "io_permission_denied";
    // ENOSPC and the rest of the io family. Kept distinct from
    // `download_error` because a full cache is a local condition the operator
    // can act on, unlike a remote failure.
    if (ContainsNoCase(error, "ENOSPC") || ContainsNoCase(error, "cache is full") ||
        ContainsNoCase(error, "Input/output error") || ContainsNoCase(error, "io error")) {
        return "io_error";
    }
    return "download_error";
}

uint64_t BackgroundDownloadDelayMs(const DownloadConfig& config) {
    const uint64_t base = config.delay > 0 ? static_cast<uint64_t>(config.delay) : 0;
    // At least one, because it is a modulus.
    const uint64_t extra = config.delay_extra > 1 ? static_cast<uint64_t>(config.delay_extra) : 1;

    // The sub-second part of the wall clock, as Rust's `subsec_nanos` is.
    //
    // Nanoseconds specifically, not microseconds scaled up: a microsecond
    // reading multiplied by 1000 is always a multiple of 1000, so `% extra`
    // would collapse to a constant for any `extra` dividing 1000 -- and even
    // the default of 30 would only ever yield {0, 10, 20}. The jitter exists
    // to stop every layer of an image starting in the same second, so a
    // biased source defeats the whole point.
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t nanos = static_cast<uint64_t>(ts.tv_nsec);
    return (base + (nanos % extra)) * 1000ULL;
}

uint64_t RetryBackoffMs(uint32_t attempt, const std::string& seed) {
    const uint32_t shift = attempt > 0 ? attempt - 1 : 0;
    // Rust's `checked_shl(..).unwrap_or(u64::MAX).min(30)`: a shift past the
    // width saturates rather than wrapping to a short delay.
    uint64_t base = shift >= 64 ? 30ULL : (1ULL << shift);
    if (base > 30ULL) base = 30ULL;

    // FNV-1a over the seed, then the attempt's little-endian bytes. Seeded
    // rather than random so two layers failing together do not retry in
    // lockstep while one layer's schedule stays reproducible.
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < seed.size(); ++i) {
        hash = (hash ^ static_cast<uint64_t>(static_cast<uint8_t>(seed[i]))) *
               0x00000100000001b3ULL;
    }
    for (int i = 0; i < 4; ++i) {
        const uint8_t byte = static_cast<uint8_t>((attempt >> (8 * i)) & 0xFF);
        hash = (hash ^ static_cast<uint64_t>(byte)) * 0x00000100000001b3ULL;
    }
    return base * 1000ULL + (hash % 1000ULL);
}

// ---------------------------------------------------------------------------
// BkSemaphore
// ---------------------------------------------------------------------------

bool BkSemaphore::Acquire() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        // Checked before the permit count: a closed semaphore must fail even
        // when permits are available, so a drain cannot be outrun by a late
        // acquirer.
        if (closed_) return false;
        if (permits_ > 0) {
            --permits_;
            return true;
        }
        cv_.wait(lk);
    }
}

void BkSemaphore::Release() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++permits_;
    }
    cv_.notify_one();
}

void BkSemaphore::Close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    // Every waiter, and permanently: unlike a notification pulse this state
    // persists, so a thread that blocks after the close still fails rather
    // than waiting for a permit nobody will release.
    cv_.notify_all();
}

bool BkSemaphore::IsClosed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
}

// ---------------------------------------------------------------------------
// FairReadyQueue
// ---------------------------------------------------------------------------

void FairReadyQueue::Push(const core::Optional<std::string>& device_key,
                          const std::shared_ptr<BkDownloadTask>& item) {
    for (size_t i = 0; i < groups_.size(); ++i) {
        const bool same = groups_[i].device_key.has_value() == device_key.has_value() &&
                          (!device_key.has_value() || *groups_[i].device_key == *device_key);
        if (same) {
            groups_[i].items.push_back(item);
            return;
        }
    }
    Group group;
    group.device_key = device_key;
    group.items.push_back(item);
    groups_.push_back(group);
}

bool FairReadyQueue::Pop(std::shared_ptr<BkDownloadTask>* out) {
    if (groups_.empty()) return false;

    Group group = groups_.front();
    groups_.pop_front();
    if (group.items.empty()) {
        // Matching Rust's `queue.pop_front()?`: an empty front group ends the
        // pop and is discarded rather than skipped past. Groups are never
        // left empty, so this does not arise.
        return false;
    }

    if (out != 0) *out = group.items.front();
    group.items.pop_front();
    // Rotated to the back only when it still has work, which is what makes
    // the drain round-robin instead of draining one device at a time.
    if (!group.items.empty()) groups_.push_back(group);
    return true;
}

void FairReadyQueue::Clear() { groups_.clear(); }

size_t FairReadyQueue::Size() const {
    size_t total = 0;
    for (size_t i = 0; i < groups_.size(); ++i) total += groups_[i].items.size();
    return total;
}

size_t FairReadyQueue::RemoveAll(const std::shared_ptr<BkDownloadTask>& task) {
    size_t removed = 0;
    for (size_t i = 0; i < groups_.size();) {
        std::deque<std::shared_ptr<BkDownloadTask> >& items = groups_[i].items;
        for (std::deque<std::shared_ptr<BkDownloadTask> >::iterator it = items.begin();
             it != items.end();) {
            if (*it == task) {
                it = items.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (items.empty()) {
            groups_.erase(groups_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    return removed;
}

// ---------------------------------------------------------------------------
// BkDownloadScheduler
// ---------------------------------------------------------------------------

BkDownloadScheduler::BkDownloadScheduler()
    : max_inflight_blocks_(1),
      chunk_size_(0),
      hedge_timeout_ms_(kBkDefaultHedgeTimeoutMs),
      running_(false),
      closed_(false),
      startup_waiting_(0),
      active_(0),
      override_mismatch_warned_(false),
      chunk_clamp_warned_(false) {}

std::shared_ptr<BkDownloadScheduler> BkDownloadScheduler::New(size_t max_inflight_blocks,
                                                                 size_t max_concurrent_files,
                                                                 uint32_t chunk_size,
                                                                 uint64_t hedge_timeout_ms) {
    std::shared_ptr<BkDownloadScheduler> s(new BkDownloadScheduler());
    const size_t files = max_concurrent_files > 0 ? max_concurrent_files : 1;

    s->max_inflight_blocks_ = max_inflight_blocks > 0 ? max_inflight_blocks : 1;
    s->chunk_size_ = chunk_size;
    s->hedge_timeout_ms_ = hedge_timeout_ms > 0 ? hedge_timeout_ms : kBkDefaultHedgeTimeoutMs;
    s->running_.store(true);

    s->file_slots_.reset(new BkSemaphore(files));
    s->block_slots_.reset(new BkSemaphore(s->max_inflight_blocks_));
    // At most a quarter of the file budget runs startup packs, so regular
    // downloads keep three quarters -- and always one slot when the budget
    // is 1.
    const size_t startup = (files / 4) > 0 ? (files / 4) : 1;
    s->startup_slots_.reset(new BkSemaphore(startup));

    // The dispatcher counts as active work, so a shutdown waits for it too.
    s->ActiveInc();

    // Sized to the file budget. A job only reaches the queue once its
    // dispatcher already owns a file permit, so there is always a free worker
    // for it and the queue never becomes a second, unbounded backlog.
    for (size_t i = 0; i < files; ++i) {
        s->workers_.push_back(std::thread(&BkDownloadScheduler::WorkerLoop, s.get()));
    }
    s->dispatcher_ = std::thread(&BkDownloadScheduler::DispatchLoop, s.get());
    return s;
}

BkDownloadScheduler::~BkDownloadScheduler() {
    Shutdown();
    if (dispatcher_.joinable()) dispatcher_.join();
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].joinable()) workers_[i].join();
    }
    JoinAux();
}

void BkDownloadScheduler::ActiveInc() { active_.fetch_add(1); }

void BkDownloadScheduler::ActiveDec() {
    active_.fetch_sub(1);
    std::lock_guard<std::mutex> lk(idle_mu_);
    // Unconditional: a waiter that sampled a non-zero count before sleeping
    // would otherwise miss the transition.
    idle_cv_.notify_all();
}

void BkDownloadScheduler::Stop() {
    {
        std::lock_guard<std::mutex> lk(closed_mu_);
        closed_ = true;
    }
    running_.store(false);

    // Closed, not merely signalled: a thread already blocked on a permit has
    // no other way to learn that no permit is coming.
    file_slots_->Close();
    block_slots_->Close();
    startup_slots_->Close();

    {
        std::lock_guard<std::mutex> lk(ready_mu_);
        ready_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        jobs_cv_.notify_all();
    }
}

void BkDownloadScheduler::Shutdown() {
    Stop();

    // Every caller performs the same idempotent wait, so cancelling one
    // shutdown cannot strand a later caller behind an abandoned leader.
    //
    // Unbounded, as Rust's is. Its worst case is one in-flight source read:
    // a synchronous refill cannot be cancelled, so the drain inherits the
    // transport's own timeout.
    {
        std::unique_lock<std::mutex> lk(idle_mu_);
        while (active_.load() != 0) {
            idle_cv_.wait_for(lk, std::chrono::milliseconds(kBkPollIntervalMs));
        }
    }

    {
        std::lock_guard<std::mutex> lk(ready_mu_);
        ready_.Clear();
    }
    {
        std::lock_guard<std::mutex> lk(tasks_mu_);
        tasks_.clear();
    }
}

bool BkDownloadScheduler::IsRegistered(const std::string& cache_id) const {
    std::lock_guard<std::mutex> lk(tasks_mu_);
    return tasks_.find(cache_id) != tasks_.end();
}

size_t BkDownloadScheduler::RegisteredCount() const {
    std::lock_guard<std::mutex> lk(tasks_mu_);
    return tasks_.size();
}

size_t BkDownloadScheduler::StartupRegisteredCount() const {
    std::lock_guard<std::mutex> lk(startup_mu_);
    return startup_tasks_.size();
}

bool BkDownloadScheduler::IsClosed() const {
    std::lock_guard<std::mutex> lk(closed_mu_);
    return closed_;
}

void BkDownloadScheduler::WarnOverrideMismatchOnce(size_t requested) {
    if (override_mismatch_warned_.exchange(true)) return;
    // Numbers only, once per scheduler: a per-image override cannot resize a
    // cap that bounds scratch memory for every task.
    AGENTENV_WARN("error_category=max_inflight_blocks_override_ignored"
                  << " requested_max_inflight_blocks=" << requested
                  << " scheduler_max_inflight_blocks=" << max_inflight_blocks_
                  << " ignoring per-image maxInflightBlocks override");
}

void BkDownloadScheduler::WarnChunkClampOnce(uint64_t requested) {
    if (chunk_clamp_warned_.exchange(true)) return;
    AGENTENV_WARN("error_category=block_size_override_clamped"
                  << " requested_block_size=" << requested
                  << " scheduler_block_size=" << chunk_size_
                  << " clamping per-image blockSize override");
}

void BkDownloadScheduler::RemoveTask(const std::shared_ptr<BkDownloadTask>& task) {
    if (!task) return;
    std::lock_guard<std::mutex> lk(tasks_mu_);
    const std::map<std::string, std::shared_ptr<BkDownloadTask> >::iterator found =
        tasks_.find(task->cache_id);
    // Identity, not just the key: a later submission for the same cache id is
    // a different task and must survive this one's teardown.
    if (found != tasks_.end() && found->second == task) tasks_.erase(found);
}

void BkDownloadScheduler::PushReady(const std::shared_ptr<BkDownloadTask>& task) {
    {
        std::lock_guard<std::mutex> lk(ready_mu_);
        ready_.Push(task->device_key, task);
    }
    ready_cv_.notify_one();
}

void BkDownloadScheduler::EnqueueJob(const Job& job) {
    {
        std::lock_guard<std::mutex> lk(jobs_mu_);
        jobs_.push_back(job);
    }
    jobs_cv_.notify_one();
}

void BkDownloadScheduler::InterruptibleSleep(uint64_t duration_ms) {
    const uint64_t deadline = NowMs() + duration_ms;
    for (;;) {
        const uint64_t now = NowMs();
        if (!running_.load() || now >= deadline) return;
        uint64_t slice = deadline - now;
        if (slice > kBkPollIntervalMs) slice = kBkPollIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    }
}

void BkDownloadScheduler::SpawnAux(const std::shared_ptr<BkDownloadTask>& task,
                                   bool timer_phase, bool wait_pending) {
    AuxThread entry;
    entry.done.reset(new std::atomic<bool>(false));

    BkDownloadScheduler* self = this;
    std::shared_ptr<BkDownloadTask> captured = task;
    std::shared_ptr<std::atomic<bool> > done = entry.done;

    entry.thread.reset(new std::thread([self, captured, timer_phase, wait_pending, done]() {
        if (timer_phase) {
            self->WaitThenMarkReady(captured);
        } else {
            if (wait_pending) {
                // Bounded: without the fallback a lost notification would
                // park the task forever.
                std::unique_lock<std::mutex> lk(self->pending_mu_);
                self->pending_cv_.wait_for(
                    lk, std::chrono::milliseconds(kBkPendingWaitFallbackMs));
            }
            if (self->running_.load()) {
                self->PushReady(captured);
            } else {
                self->RemoveTask(captured);
            }
            self->ActiveDec();
        }
        done->store(true);
    }));

    std::lock_guard<std::mutex> lk(aux_mu_);
    aux_.push_back(entry);
}

void BkDownloadScheduler::ReapAux() {
    std::lock_guard<std::mutex> lk(aux_mu_);
    for (size_t i = 0; i < aux_.size();) {
        if (aux_[i].done->load()) {
            if (aux_[i].thread->joinable()) aux_[i].thread->join();
            aux_.erase(aux_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

void BkDownloadScheduler::JoinAux() {
    std::vector<AuxThread> pending;
    {
        std::lock_guard<std::mutex> lk(aux_mu_);
        pending.swap(aux_);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i].thread->joinable()) pending[i].thread->join();
    }
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

core::Expected<core::Unit, std::string> BkDownloadScheduler::Submit(
    FileCacheBackend* backend, const std::vector<BkDownloadRequest>& requests) {
    if (backend == 0) {
        return core::make_unexpected(std::string("background download submit without backend"));
    }
    ReapAux();

    std::set<std::string> seen;
    for (size_t i = 0; i < requests.size(); ++i) {
        const BkDownloadRequest& request = requests[i];
        if (!request.file) continue;

        const std::string cache_id = request.file->CacheId();
        // Within one batch: the same layer named twice is one download.
        if (!seen.insert(cache_id).second) continue;

        std::shared_ptr<VirtualFile> source;
        uint64_t source_size = 0;
        const core::Expected<core::Unit, std::string> snapshot =
            request.file->BackgroundSourceSnapshot(&source, &source_size);
        if (!snapshot.ok()) {
            // Skipped, not failed: the foreground path still refills on
            // demand, so a warm-up that cannot start must not block an open.
            WarnSubmitSkipped(cache_id, snapshot.error());
            continue;
        }

        DownloadConfig config = request.config;
        if (config.concurrency < 1) config.concurrency = 1;
        if (config.try_cnt < 1) config.try_cnt = 1;
        if (config.max_inflight_blocks != max_inflight_blocks_) {
            WarnOverrideMismatchOnce(config.max_inflight_blocks);
        }

        const core::Expected<bool, std::string> complete =
            request.file->BackgroundIsComplete(source_size);
        if (!complete.ok()) {
            WarnSubmitSkipped(cache_id, complete.error());
            continue;
        }
        // Nothing to do: every block is already cached.
        if (complete.value()) continue;

        std::string cache_key;
        if (!backend->CacheKeyOf(cache_id, &cache_key)) {
            AGENTENV_WARN("error_category=cache_entry_missing cache_id="
                          << cache_id << " skipping background download submission");
            continue;
        }

        // `download.blockSize` is the *background* granularity; the cache
        // stores blocks in its own smaller size. Aligned down to whole cache
        // blocks, then clamped to the scheduler's global chunk size so the
        // scratch budget holds even for an override.
        uint64_t cache_block_size = request.file->BackgroundBlockSize();
        if (cache_block_size < 1) cache_block_size = 1;
        const uint64_t max_chunk_blocks =
            static_cast<uint64_t>(chunk_size_) / cache_block_size;
        uint64_t blocks_per_chunk = static_cast<uint64_t>(config.block_size) / cache_block_size;
        const uint64_t ceiling = max_chunk_blocks > 0 ? max_chunk_blocks : 1;
        if (blocks_per_chunk > ceiling) {
            WarnChunkClampOnce(static_cast<uint64_t>(config.block_size));
            blocks_per_chunk = max_chunk_blocks;
        }
        if (blocks_per_chunk > 0xFFFFFFFFULL) blocks_per_chunk = 0xFFFFFFFFULL;
        if (blocks_per_chunk < 1) blocks_per_chunk = 1;

        std::shared_ptr<BkDownloadTask> task(new BkDownloadTask());
        task->cache_id = cache_id;
        task->cache_key = cache_key;
        task->source = source;
        task->source_size = source_size;
        task->config = config;
        task->blocks_per_chunk = static_cast<uint32_t>(blocks_per_chunk);
        task->device_key = request.device_key;
        task->backend = backend;

        {
            // The closed check and the registry insertion are one critical
            // section against shutdown: once closed, no task may slip past
            // the drain.
            std::lock_guard<std::mutex> closed_lk(closed_mu_);
            if (closed_) return core::make_unexpected(std::string(kBkSubmitClosedMessage));

            std::lock_guard<std::mutex> tasks_lk(tasks_mu_);
            // Already registered: the live task covers this layer.
            if (tasks_.find(cache_id) != tasks_.end()) continue;
            tasks_[cache_id] = task;
        }

        ActiveInc();
        SpawnAux(task, true, false);
    }
    return core::Unit();
}

core::Expected<StartupPackHandle, std::string> BkDownloadScheduler::SubmitStartupPack(
    const StartupPackSubmission& submission) {
    std::lock_guard<std::mutex> closed_lk(closed_mu_);
    if (closed_) return core::make_unexpected(std::string(kBkSubmitClosedMessage));

    std::shared_ptr<BkDownloadScheduler> self = shared_from_this();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(startup_mu_);
        const std::map<std::string, std::shared_ptr<StartupPackTask> >::iterator found =
            startup_tasks_.find(submission.task_key);

        if (found != startup_tasks_.end() && !found->second->IsTerminal()) {
            // Attached to the live task: concurrent resumes of one snapshot
            // share a single download. The handle below takes a reference,
            // so this is the one path that retains.
            found->second->Retain();
        } else {
            // Absent, or terminal and therefore never re-attached: a fresh
            // task starts with one holder, which the returned handle
            // inherits rather than adding to.
            std::shared_ptr<StartupPackTask> task = StartupPackTask::New(submission);
            startup_tasks_[submission.task_key] = task;
            startup_ready_.push_back(task);
            startup_waiting_.fetch_add(1);
            queued = true;
        }
    }
    if (queued) ready_cv_.notify_one();

    return StartupPackHandle(self, submission.task_key);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void BkDownloadScheduler::DispatchLoop() {
    for (;;) {
        ReapAux();
        if (!running_.load()) break;

        // Startup packs dispatch ahead of background downloads: a resume is
        // latency-critical, a warm-up is not.
        std::shared_ptr<StartupPackTask> startup;
        {
            std::lock_guard<std::mutex> lk(startup_mu_);
            if (!startup_ready_.empty()) {
                startup = startup_ready_.front();
                startup_ready_.pop_front();
            }
        }

        if (startup) {
            if (startup->FailIfPastDeadline()) {
                startup_waiting_.fetch_sub(1);
                continue;
            }
            BkPermit startup_permit(startup_slots_.get(), startup_slots_->Acquire());
            if (!startup_permit.held()) break;
            BkPermit file_permit(file_slots_.get(), file_slots_->Acquire());
            if (!file_permit.held()) break;
            if (!running_.load()) break;

            // About to run: it no longer needs backgrounds to yield.
            startup_waiting_.fetch_sub(1);
            ActiveInc();

            Job job;
            job.is_startup = true;
            job.startup = startup;
            job.holds_startup = true;
            job.holds_file = true;
            startup_permit.Disarm();
            file_permit.Disarm();
            EnqueueJob(job);
            continue;
        }

        std::shared_ptr<BkDownloadTask> task;
        {
            std::unique_lock<std::mutex> lk(ready_mu_);
            if (!ready_.Pop(&task)) {
                ready_cv_.wait_for(lk, std::chrono::milliseconds(kBkPollIntervalMs));
                continue;
            }
        }

        BkPermit file_permit(file_slots_.get(), file_slots_->Acquire());
        if (!file_permit.held()) break;
        // Shutdown clears the registry itself, so a task popped and then
        // abandoned here needs no cleanup.
        if (!running_.load()) break;

        ActiveInc();
        Job job;
        job.is_startup = false;
        job.task = task;
        job.holds_file = true;
        file_permit.Disarm();
        EnqueueJob(job);
    }
    ActiveDec();
}

void BkDownloadScheduler::WorkerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(jobs_mu_);
            while (jobs_.empty() && running_.load()) {
                jobs_cv_.wait_for(lk, std::chrono::milliseconds(kBkPollIntervalMs));
            }
            if (jobs_.empty()) {
                if (!running_.load()) return;
                continue;
            }
            job = jobs_.front();
            jobs_.pop_front();
        }

        // The dispatcher acquired these; releasing them is this worker's job,
        // and it must happen before any requeue wait below.
        BkPermit file_permit(file_slots_.get(), job.holds_file);
        BkPermit startup_permit(startup_slots_.get(), job.holds_startup);

        if (job.is_startup) {
            // The result is already recorded as the task's terminal phase.
            //
            // Deviation: Rust hands the scheduler's `block_slots` semaphore
            // to the manifest workers so their scratch shares the global
            // budget. This port passes the cap as a worker-count bound
            // instead, so the budget is enforced per task rather than
            // globally; the startup gate floor still bounds concurrency
            // across tasks.
            RunStartupPackTask(job.startup, &running_, MaxInflightBlocks());
            ActiveDec();
            continue;
        }

        const RunOutcome outcome = RunTask(job.task);
        if (outcome == kRunDone) {
            RemoveTask(job.task);
            ActiveDec();
            continue;
        }

        // Requeued, keeping both the registration and the active count.
        //
        // Deviation: Rust adds a *second* active count here for the requeue
        // future while its defused run guard drops the first without
        // decrementing it, so each requeue leaks one count and `shutdown`
        // (which waits for the count to reach zero) would never return. The
        // count is transferred instead of duplicated.
        SpawnAux(job.task, false, outcome == kRunWaitPending);
    }
}

void BkDownloadScheduler::WaitThenMarkReady(const std::shared_ptr<BkDownloadTask>& task) {
    // The timer phase holds no execution resource: readiness and the
    // configured delay are waited out before the task competes for a slot.
    if (task->device_key.has_value()) {
        DownloadGate::WaitSandboxReady(*task->device_key, kSandboxReadyFallbackSec, &running_);
    }
    InterruptibleSleep(BackgroundDownloadDelayMs(task->config));

    if (running_.load()) {
        std::unique_lock<std::mutex> lk(ready_mu_);
        // Re-checked under the queue lock so a task cannot land in the queue
        // after the drain has cleared it.
        if (running_.load()) {
            ready_.Push(task->device_key, task);
            lk.unlock();
            ready_cv_.notify_one();
            ActiveDec();
            return;
        }
    }

    // Never became ready, so its registration goes with it.
    RemoveTask(task);
    ActiveDec();
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

BkDownloadScheduler::RunOutcome BkDownloadScheduler::RunTask(
    const std::shared_ptr<BkDownloadTask>& task) {
    std::unique_ptr<BkThrottle> throttle;
    if (task->config.max_mbps > 0) {
        throttle.reset(new BkThrottle());
        throttle->limit_bps =
            static_cast<uint64_t>(task->config.max_mbps) * 1024ULL * 1024ULL;
    }

    const uint32_t attempts =
        task->config.try_cnt > 0 ? static_cast<uint32_t>(task->config.try_cnt) : 1;

    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        if (!running_.load()) return kRunDone;

        // Reopened every attempt: the entry may have been evicted while the
        // task was pending, or by the very ENOSPC that failed the last one.
        const core::Expected<std::shared_ptr<CachedFile>, std::string> file =
            task->backend->OpenFile(task->cache_key, task->source, task->source_size, 0);
        if (!file.ok()) {
            if (attempt + 1 < attempts) {
                InterruptibleSleep(RetryBackoffMs(attempt + 1, task->cache_id));
                continue;
            }
            AGENTENV_WARN("error_category=" << DownloadErrorCategory(file.error())
                                            << " background cache download could not open"
                                               " cached file");
            return kRunDone;
        }

        const core::Expected<std::vector<std::pair<uint64_t, uint32_t> >, std::string> chunks =
            file.value()->ActionableBackgroundChunks(task->source_size,
                                                     task->blocks_per_chunk);
        if (!chunks.ok()) {
            AGENTENV_WARN("error_category=" << DownloadErrorCategory(chunks.error())
                                            << " invalid background cache block range");
            return kRunDone;
        }

        if (chunks.value().empty()) {
            // Completion is ALWAYS judged on the raw bitmap, never on a
            // filtered view of it: a task that stopped because its holes
            // belong to someone else is not finished.
            const core::Expected<bool, std::string> complete =
                file.value()->BackgroundIsComplete(task->source_size);
            if (complete.ok() && complete.value()) return kRunDone;
            return kRunWaitPending;
        }

        std::string error;
        const ChunksOutcome outcome =
            DownloadChunks(task, file.value(), chunks.value(), throttle.get(), &error);
        if (!running_.load()) return kRunDone;
        if (outcome == kChunksDone) return kRunDone;
        if (outcome == kChunksYieldStartup) return kRunYieldStartup;

        const char* category = DownloadErrorCategory(error);
        if (attempt + 1 < attempts) {
            AGENTENV_WARN("attempt=" << (attempt + 1) << " error_category=" << category
                                     << " background cache download will retry missing blocks");
            // Partial progress is kept: the next attempt re-enumerates the
            // missing chunks, so a failure costs only what it did not fetch.
            InterruptibleSleep(RetryBackoffMs(attempt + 1, task->cache_id));
        } else {
            AGENTENV_WARN("error_category=" << category
                                            << " background cache download terminally failed");
        }
    }
    return kRunDone;
}

namespace {

/// Shared state of one task's in-flight chunk threads.
struct ChunkFleet {
    std::mutex mu;
    std::condition_variable cv;
    size_t in_flight;
    std::string first_error;
    bool has_error;

    ChunkFleet() : in_flight(0), has_error(false) {}
};

}  // namespace

BkDownloadScheduler::ChunksOutcome BkDownloadScheduler::DownloadChunks(
    const std::shared_ptr<BkDownloadTask>& task, const std::shared_ptr<CachedFile>& file,
    const std::vector<std::pair<uint64_t, uint32_t> >& chunks, BkThrottle* throttle,
    std::string* first_error) {
    const size_t concurrency = task->config.concurrency > 0 ? task->config.concurrency : 1;

    ChunkFleet fleet;
    std::vector<std::thread> threads;
    size_t next = 0;
    bool yield = false;

    for (;;) {
        // Top up to the task's concurrency.
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(fleet.mu);
                if (fleet.has_error || !running_.load() || fleet.in_flight >= concurrency) break;
            }
            // A queued startup pack preempts background downloads at chunk
            // boundaries, so a long layer cannot delay a resume.
            if (StartupWaiting()) {
                yield = true;
                break;
            }
            if (next >= chunks.size()) break;

            const std::pair<uint64_t, uint32_t> chunk = chunks[next++];
            {
                std::lock_guard<std::mutex> lk(fleet.mu);
                ++fleet.in_flight;
            }
            BkDownloadScheduler* self = this;
            std::shared_ptr<CachedFile> captured_file = file;
            std::shared_ptr<VirtualFile> source = task->source;
            const uint64_t source_size = task->source_size;
            threads.push_back(std::thread([self, captured_file, source, source_size, chunk,
                                           throttle, &fleet]() {
                const core::Expected<uint64_t, std::string> read = self->RefillChunkWithHedge(
                    captured_file, source, source_size, chunk.first, chunk.second);
                if (read.ok()) self->ThrottleAfterBlock(throttle, read.value());
                std::lock_guard<std::mutex> lk(fleet.mu);
                if (!read.ok() && !fleet.has_error) {
                    // Only the first error is kept, as Rust's
                    // `get_or_insert` does: the retry loop re-enumerates
                    // anyway, so later failures add nothing.
                    fleet.has_error = true;
                    fleet.first_error = read.error();
                }
                --fleet.in_flight;
                fleet.cv.notify_all();
            }));
        }

        if (yield) break;

        std::unique_lock<std::mutex> lk(fleet.mu);
        if (fleet.in_flight == 0) break;
        fleet.cv.wait_for(lk, std::chrono::milliseconds(kBkPollIntervalMs));
    }

    // Joined rather than abandoned.
    //
    // Deviation: Rust returns from `YieldStartup` immediately, and dropping
    // its JoinSet aborts the in-flight chunk futures at their next await --
    // which releases their refill elections and discards the partial work. A
    // thread cannot be aborted, so the yield instead waits out the chunks
    // already dispatched. The startup pack therefore waits up to one chunk
    // read longer, and no fetched bytes are thrown away.
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    if (fleet.has_error) {
        if (first_error != 0) *first_error = fleet.first_error;
        return kChunksFailed;
    }
    if (yield) return kChunksYieldStartup;
    return kChunksDone;
}

core::Expected<uint64_t, std::string> BkDownloadScheduler::RefillChunkWithHedge(
    const std::shared_ptr<CachedFile>& file, const std::shared_ptr<VirtualFile>& source,
    uint64_t source_size, uint64_t start_block, uint32_t len_blocks) {
    // The bounded I/O slot first, then the foreground-pressure admission. The
    // order matters: a task that waited for a slot must not have pre-admitted
    // itself past foreground pressure that arrived meanwhile.
    BkPermit slot(block_slots_.get(), block_slots_->Acquire());
    if (!slot.held()) {
        return core::make_unexpected(std::string("background cache block slots closed"));
    }

    const std::shared_ptr<BkReadPermit> gate = DownloadGate::GateBlockRead(&running_);
    if (!gate) {
        return core::make_unexpected(
            std::string("background cache download canceled before chunk dispatch"));
    }

    // Deviation: Rust wraps this call in `tokio::time::timeout(hedge_timeout)`
    // and reissues the request up to `HEDGE_MAX_ATTEMPTS` times, so a
    // straggler connection cannot hold a block slot for the whole transport
    // timeout. A synchronous call cannot be cancelled: abandoning the thread
    // would keep the slot held *past* the hedge budget while a second request
    // took another one, which is strictly worse than waiting. So the per-read
    // budget is delegated to the transport's own timeout, and the hedge retry
    // is not ported -- note that Rust retries only on its timeout, never on
    // the refill's own error, so the error path here is unchanged.
    //
    // The slot and the gate permit are released as this scope ends, before
    // the task-level retry backoff sleeps.
    return file->BackgroundRefillRange(source, source_size, start_block, len_blocks);
}

void BkDownloadScheduler::ThrottleAfterBlock(BkThrottle* throttle, uint64_t bytes) {
    if (throttle == 0 || throttle->limit_bps == 0) return;

    uint64_t sleep_ms = 0;
    {
        std::lock_guard<std::mutex> lk(throttle->mu);
        throttle->downloaded += bytes;
        // Where the task *should* be in time if it had run exactly at the
        // limit, minus where it actually is. Measured against the task's own
        // start so a slow stretch earns credit rather than being clawed back.
        const double expected = static_cast<double>(throttle->downloaded) /
                                static_cast<double>(throttle->limit_bps);
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double> >(
                std::chrono::steady_clock::now() - throttle->started)
                .count();
        const double overdue = expected - elapsed;
        if (overdue > 0.0) sleep_ms = static_cast<uint64_t>(overdue * 1000.0);
    }
    // Outside the lock: holding it would serialise the other chunk threads
    // behind this one's sleep.
    if (sleep_ms > 0) InterruptibleSleep(sleep_ms);
}

// ---------------------------------------------------------------------------
// StartupPackHandle
// ---------------------------------------------------------------------------

StartupPackHandle::StartupPackHandle(const std::shared_ptr<BkDownloadScheduler>& scheduler,
                                     const std::string& task_key)
    : scheduler_(scheduler), task_key_(task_key) {}

StartupPackHandle::StartupPackHandle(const StartupPackHandle& other)
    : scheduler_(other.scheduler_), task_key_(other.task_key_) {
    // A copy is another interested party, so it takes its own reference.
    const std::shared_ptr<StartupPackTask> task = Task();
    if (task) task->Retain();
}

StartupPackHandle& StartupPackHandle::operator=(const StartupPackHandle& other) {
    if (this == &other) return *this;
    StartupPackHandle copy(other);
    // The old reference is given up through the temporary's destructor, so
    // self-assignment and chained assignment cannot double-release.
    std::swap(scheduler_, copy.scheduler_);
    std::swap(task_key_, copy.task_key_);
    return *this;
}

std::shared_ptr<StartupPackTask> StartupPackHandle::Task() const {
    if (!scheduler_) return std::shared_ptr<StartupPackTask>();
    std::lock_guard<std::mutex> lk(scheduler_->startup_mu_);
    const std::map<std::string, std::shared_ptr<StartupPackTask> >::const_iterator found =
        scheduler_->startup_tasks_.find(task_key_);
    if (found == scheduler_->startup_tasks_.end()) {
        return std::shared_ptr<StartupPackTask>();
    }
    return found->second;
}

StartupPackPhase StartupPackHandle::Phase() const {
    const std::shared_ptr<StartupPackTask> task = Task();
    // A handle to something the scheduler has forgotten cannot report
    // progress, so it reports failure rather than a stale phase.
    if (!task) return kStartupPackFailed;
    return task->Phase();
}

core::Optional<StartupPackStatsSnapshot> StartupPackHandle::Stats() const {
    const std::shared_ptr<StartupPackTask> task = Task();
    if (!task) return core::Optional<StartupPackStatsSnapshot>();
    return core::Optional<StartupPackStatsSnapshot>(task->Stats());
}

StartupPackPhase StartupPackHandle::WaitTerminal() {
    const std::shared_ptr<StartupPackTask> task = Task();
    if (!task) return kStartupPackFailed;
    for (;;) {
        const StartupPackPhase phase = task->Phase();
        if (task->IsTerminal()) return phase;
        // Polled rather than waiting on the task's phase signal: this is a
        // test and A/B helper, and resume never blocks on it.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

StartupPackHandle::~StartupPackHandle() {
    if (!scheduler_) return;
    const std::shared_ptr<StartupPackTask> task = Task();
    if (!task) return;

    if (task->IsTerminal()) {
        // A terminal task is never re-attached -- a new submission for the
        // same key builds a fresh one -- so it is deregistered here instead
        // of sitting in the registry forever.
        std::lock_guard<std::mutex> lk(scheduler_->startup_mu_);
        const std::map<std::string, std::shared_ptr<StartupPackTask> >::iterator found =
            scheduler_->startup_tasks_.find(task_key_);
        if (found != scheduler_->startup_tasks_.end() && found->second == task) {
            scheduler_->startup_tasks_.erase(found);
        }
        return;
    }

    if (!task->Release()) return;

    // Last holder of a live task: nobody is waiting for this prefetch any
    // more, so continuing would spend bandwidth on blocks no reader will ask
    // for.
    if (!task->IsTerminal()) {
        task->Cancel();
        task->SetPhase(kStartupPackCanceled);

        size_t removed = 0;
        {
            std::lock_guard<std::mutex> lk(scheduler_->startup_mu_);
            for (std::deque<std::shared_ptr<StartupPackTask> >::iterator it =
                     scheduler_->startup_ready_.begin();
                 it != scheduler_->startup_ready_.end();) {
                if (*it == task) {
                    it = scheduler_->startup_ready_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        // Dropped from the queue, so the count that makes background tasks
        // yield has to come down with it.
        if (removed > 0) scheduler_->startup_waiting_.fetch_sub(removed);
    }

    {
        std::lock_guard<std::mutex> lk(scheduler_->startup_mu_);
        const std::map<std::string, std::shared_ptr<StartupPackTask> >::iterator found =
            scheduler_->startup_tasks_.find(task_key_);
        if (found != scheduler_->startup_tasks_.end() && found->second == task) {
            scheduler_->startup_tasks_.erase(found);
        }
    }

    // The pending-import set changed, so a parked background task may now
    // have work.
    {
        std::lock_guard<std::mutex> lk(scheduler_->pending_mu_);
        scheduler_->pending_cv_.notify_all();
    }
}

// ==== backend-facing submission ==========================================

core::Expected<core::Unit, std::string> SubmitBkDownloadBatch(
    FileCacheBackend* backend, const std::vector<BkDownloadRequest>& requests) {
    if (backend == 0) {
        return core::make_unexpected(std::string("background download submit without backend"));
    }
    const std::shared_ptr<BkDownloadScheduler> scheduler = backend->BkScheduler();
    // Rust reports `BkDownloadSubmitError::Closed` when the `Weak` no longer
    // upgrades; a backend without a scheduler is the same condition, and the
    // caller distinguishes it by this message.
    if (!scheduler) return core::make_unexpected(std::string(kBkSubmitClosedMessage));
    return scheduler->Submit(backend, requests);
}

core::Expected<core::Unit, std::string> SubmitBkDownload(
    FileCacheBackend* backend, std::shared_ptr<CachedFile> file,
    const DownloadConfig& config, const core::Optional<std::string>& device_key) {
    BkDownloadRequest request;
    request.file = file;
    request.config = config;
    request.device_key = device_key;
    std::vector<BkDownloadRequest> one;
    one.push_back(request);
    return SubmitBkDownloadBatch(backend, one);
}

core::Expected<StartupPackHandle, std::string> SubmitStartupPack(
    FileCacheBackend* backend, const StartupPackSubmission& submission) {
    if (backend == 0) {
        return core::make_unexpected(std::string("startup pack submit without backend"));
    }
    const std::shared_ptr<BkDownloadScheduler> scheduler = backend->BkScheduler();
    if (!scheduler) return core::make_unexpected(std::string(kBkSubmitClosedMessage));
    return scheduler->SubmitStartupPack(submission);
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
