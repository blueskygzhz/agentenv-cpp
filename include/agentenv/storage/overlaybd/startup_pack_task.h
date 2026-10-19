// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/startup_pack_task.rs — one startup
// manifest prefetch.
//
// Startup prefetch exists because a guest's first seconds touch a working set
// that is knowable in advance: fetch it in bulk up front instead of paying a
// round trip per 4 KiB fault. The manifest is that working set, recorded in
// first-need order.
#ifndef AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_TASK_H_
#define AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_TASK_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/cache_pool.h"
#include "agentenv/storage/overlaybd/pack_planner.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `MANIFEST_WORKERS` — concurrent workers for one manifest prefetch.
const size_t kManifestWorkers = 8;

/// The prefetch is only correct when the cache's block size equals
/// `kObjectBlockBytes`.
///
/// The planner emits block ids counted in `kObjectBlockBytes` units, while
/// `BackgroundRefillRange` interprets them in the cache's own block size. The
/// two are the same number by configuration -- Rust's cache defaults to
/// `256 * 1024`, which is `OBJECT_BLOCK_BYTES` -- and nothing in either layer
/// converts between them. A cache configured differently would refill the
/// wrong offsets and still report success, so the submission path checks it
/// rather than trusting the default.
bool StartupPrefetchBlockSizeMatches(uint64_t cache_block_size);

/// Rust `StartupPackLayer` — one import target of the manifest.
///
/// Bound by the caller through the normal image-service cache entry point, so
/// the cache-key transform is the same one the on-demand path uses. That is
/// what lets a prefetched block satisfy a later fault instead of occupying
/// cache space under a different name.
struct StartupPackLayer {
    std::string digest;
    uint64_t size;
    std::shared_ptr<CachedFile> file;
    /// The raw source behind `file`, for the batched refills. Kept separately
    /// because the cached file's own source may be null (a cache-only handle).
    std::shared_ptr<VirtualFile> source;

    StartupPackLayer() : size(0) {}
};

/// Rust `StartupPackSubmission`.
struct StartupPackSubmission {
    /// Dedup identity, bound to the immutable manifest description (its
    /// sha256), so concurrent resumes of one snapshot share a single task.
    std::string task_key;
    /// The manifest object itself, opened *without* the full-file cache: it is
    /// read once, and only layer blocks belong in the cache.
    std::shared_ptr<VirtualFile> pack_source;
    uint64_t pack_size;
    /// Hex sha256 of the whole manifest body, from the committed record. The
    /// manifest is trusted only after its body matches this.
    std::string index_sha256;
    uint64_t mem_virtual_size;
    /// Bottom-to-top, matching the memory image's layer order.
    std::vector<StartupPackLayer> layers;
    /// Hard bound on queueing plus downloading, so a manifest that never
    /// completes cannot hold scheduler capacity forever.
    uint64_t timeout_ms;

    StartupPackSubmission() : pack_size(0), mem_virtual_size(0), timeout_ms(0) {}
};

/// Rust `StartupPackPhase`.
enum StartupPackPhase {
    kStartupPackQueued,
    kStartupPackRunning,
    kStartupPackDone,
    kStartupPackFailed,
    kStartupPackCanceled
};

/// Rust `StartupPackStatsSnapshot`.
struct StartupPackStatsSnapshot {
    /// Source range reads issued, including retried attempts.
    uint64_t oss_gets;
    /// Bytes received, including retried attempts.
    uint64_t oss_bytes;

    StartupPackStatsSnapshot() : oss_gets(0), oss_bytes(0) {}
};

/// Declared here, defined in `bk_download.h`: Rust keeps `StartupPackHandle`
/// in the scheduler module because a handle's lifetime touches the scheduler's
/// registry and ready queue, not just the task.
class StartupPackHandle;

/// Rust `StartupPackTask`.
class StartupPackTask {
 public:
    static std::shared_ptr<StartupPackTask> New(const StartupPackSubmission& submission);

    StartupPackPhase Phase() const;
    void SetPhase(StartupPackPhase phase);
    bool IsTerminal() const;
    bool IsCanceled() const;
    void Cancel();

    /// Rust `fail_if_past_deadline` — true when the task is (now) terminal and
    /// must not run.
    bool FailIfPastDeadline();

    /// Rust `retain` / `release`.
    void Retain();
    /// True when this was the last holder and the task was not terminal.
    bool Release();

    StartupPackStatsSnapshot Stats() const;
    bool DeadlinePassed() const;

    const std::string& TaskKey() const { return task_key_; }
    const std::string& IndexSha256() const { return index_sha256_; }
    uint64_t MemVirtualSize() const { return mem_virtual_size_; }
    uint64_t PackSize() const { return pack_size_; }
    const std::vector<StartupPackLayer>& Layers() const { return layers_; }
    VirtualFile* PackSource() const { return pack_source_.get(); }

    /// Per-block progress, for a handle that wants to report it.
    uint64_t OssGets() const;
    uint64_t OssBytes() const;
    void AddStats(uint64_t gets, uint64_t bytes);

    /// Blocks the source still owes, so a caller can decide whether the
    /// prefetch is worth waiting for.
    core::Expected<uint64_t, std::string> TotalBlocks() const;

 private:
    StartupPackTask() : pack_size_(0), mem_virtual_size_(0), holders_(1),
                        canceled_(false), oss_gets_(0), oss_bytes_(0) {}

    void BumpDeadline(uint64_t timeout_ms);

    std::string task_key_;
    std::shared_ptr<VirtualFile> pack_source_;
    uint64_t pack_size_;
    std::string index_sha256_;
    uint64_t mem_virtual_size_;
    std::vector<StartupPackLayer> layers_;

    mutable std::mutex mu_;
    mutable std::condition_variable phase_cv_;
    StartupPackPhase phase_;

    std::atomic<size_t> holders_;
    std::atomic<bool> canceled_;
    std::atomic<uint64_t> oss_gets_;
    std::atomic<uint64_t> oss_bytes_;
    uint64_t deadline_ms_;
};

/// Rust `merge_runs_by_first_need`.
///
/// Merges the first-need block plan into per-layer adjacent runs, capped at
/// 64 blocks (16 MiB) so one source read covers a contiguous span. Runs are
/// returned sorted by their earliest first-need rank.
///
/// The sort is the point: the same runs cost the same request count as
/// physical order, but boot-critical blocks (the manifest's exact-order
/// prefix) land first instead of in whatever order the object happens to
/// store them.
struct StartupPackRun {
    size_t layer_idx;
    uint64_t start_block;
    uint32_t len_blocks;
    uint64_t first_need_rank;
};

std::vector<StartupPackRun> MergeRunsByFirstNeed(const PackPlan& plan);

/// Rust `run_startup_pack_task` — drive one task to completion, failure or
/// cancellation, setting its terminal phase.
///
/// `running` is the scheduler's shutdown flag and `block_slots_cap` its
/// concurrency budget; both are passed rather than reached for, so this layer
/// has no dependency on the scheduler's type.
core::Expected<core::Unit, std::string> RunStartupPackTask(
    const std::shared_ptr<StartupPackTask>& task, const std::atomic<bool>* running,
    uint32_t block_slots_cap);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_STARTUP_PACK_TASK_H_
