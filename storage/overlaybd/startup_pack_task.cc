// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/cache/startup_pack_task.rs
#include "agentenv/storage/overlaybd/startup_pack_task.h"

#include <sys/time.h>

#include <cstring>

#include <algorithm>
#include <sstream>
#include <thread>

#include "agentenv/core/digest.h"
#include "agentenv/storage/overlaybd/download_gate.h"
#include "agentenv/storage/overlaybd/startup_pack.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

uint64_t NowMs() {
    struct timeval tv;
    ::gettimeofday(&tv, 0);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

/// Rust's `RUN_BLOCKS` inside `merge_runs_by_first_need`.
///
/// 64 blocks of 256 KiB is 16 MiB per source request. Capped rather than
/// unbounded because a run is read into scratch before being published: an
/// uncapped run on a large layer would allocate a copy of the whole object.
const uint32_t kRunBlocks = 64;

}  // namespace

bool StartupPrefetchBlockSizeMatches(uint64_t cache_block_size) {
    return cache_block_size == kObjectBlockBytes;
}

// ---------------------------------------------------------------------------
// StartupPackTask
// ---------------------------------------------------------------------------

std::shared_ptr<StartupPackTask> StartupPackTask::New(
    const StartupPackSubmission& submission) {
    std::shared_ptr<StartupPackTask> task(new StartupPackTask());
    task->task_key_ = submission.task_key;
    task->pack_source_ = submission.pack_source;
    task->pack_size_ = submission.pack_size;
    task->index_sha256_ = submission.index_sha256;
    task->mem_virtual_size_ = submission.mem_virtual_size;
    task->layers_ = submission.layers;
    task->phase_ = kStartupPackQueued;
    // The deadline starts now, covering *queueing* as well as downloading:
    // a task that waits an hour for a slot has already missed the boot it was
    // meant to accelerate.
    task->BumpDeadline(submission.timeout_ms);
    return task;
}

void StartupPackTask::BumpDeadline(uint64_t timeout_ms) {
    deadline_ms_ = NowMs() + timeout_ms;
}

StartupPackPhase StartupPackTask::Phase() const {
    std::lock_guard<std::mutex> lk(mu_);
    return phase_;
}

void StartupPackTask::SetPhase(StartupPackPhase phase) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        phase_ = phase;
    }
    // Every waiter: several devices may share one manifest task, and all of
    // them are watching for it to finish.
    phase_cv_.notify_all();
}

bool StartupPackTask::IsTerminal() const {
    const StartupPackPhase phase = Phase();
    return phase == kStartupPackDone || phase == kStartupPackFailed ||
           phase == kStartupPackCanceled;
}

bool StartupPackTask::IsCanceled() const { return canceled_.load(); }

void StartupPackTask::Cancel() { canceled_.store(true); }

bool StartupPackTask::DeadlinePassed() const { return NowMs() >= deadline_ms_; }

bool StartupPackTask::FailIfPastDeadline() {
    if (IsTerminal()) return true;
    if (DeadlinePassed()) {
        SetPhase(kStartupPackFailed);
        return true;
    }
    return false;
}

void StartupPackTask::Retain() { holders_.fetch_add(1); }

bool StartupPackTask::Release() {
    // `fetch_sub` returns the value *before* the decrement, so 1 means this
    // call took it to zero.
    const size_t before = holders_.fetch_sub(1);
    return before == 1 && !IsTerminal();
}

StartupPackStatsSnapshot StartupPackTask::Stats() const {
    StartupPackStatsSnapshot out;
    out.oss_gets = oss_gets_.load();
    out.oss_bytes = oss_bytes_.load();
    return out;
}

uint64_t StartupPackTask::OssGets() const { return oss_gets_.load(); }
uint64_t StartupPackTask::OssBytes() const { return oss_bytes_.load(); }

void StartupPackTask::AddStats(uint64_t gets, uint64_t bytes) {
    oss_gets_.fetch_add(gets);
    oss_bytes_.fetch_add(bytes);
}

core::Expected<uint64_t, std::string> StartupPackTask::TotalBlocks() const {
    uint64_t total = 0;
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (!layers_[i].file) continue;
        const uint64_t block_size = layers_[i].file->BackgroundBlockSize();
        if (block_size == 0) {
            return core::make_unexpected(std::string("cache block size is zero"));
        }
        total += (layers_[i].size + block_size - 1) / block_size;
    }
    return total;
}

// ---------------------------------------------------------------------------
// merge_runs_by_first_need
// ---------------------------------------------------------------------------

namespace {

struct RunItem {
    size_t layer_idx;
    uint64_t block;
    uint32_t len;
    uint64_t rank;
};

bool ByLayerThenBlock(const RunItem& a, const RunItem& b) {
    if (a.layer_idx != b.layer_idx) return a.layer_idx < b.layer_idx;
    return a.block < b.block;
}

bool ByRank(const StartupPackRun& a, const StartupPackRun& b) {
    return a.first_need_rank < b.first_need_rank;
}

}  // namespace

std::vector<StartupPackRun> MergeRunsByFirstNeed(const PackPlan& plan) {
    std::vector<RunItem> items;
    items.reserve(plan.blocks.size());
    for (size_t rank = 0; rank < plan.blocks.size(); ++rank) {
        const PlannedBlock& block = plan.blocks[rank];
        if (static_cast<size_t>(block.object) >= plan.objects.size()) continue;
        RunItem item;
        item.layer_idx = plan.objects[block.object].layer;
        item.block = block.block_id;
        item.len = 1;
        // Position in the plan *is* the first-need rank: the planner emits
        // blocks in first-need order.
        item.rank = rank;
        items.push_back(item);
    }

    // Sorted by address to find adjacency. Rust uses `sort_unstable_by_key`;
    // stability is irrelevant because `(layer, block)` is unique.
    std::sort(items.begin(), items.end(), ByLayerThenBlock);

    std::vector<StartupPackRun> runs;
    for (size_t i = 0; i < items.size(); ++i) {
        const RunItem& item = items[i];
        if (!runs.empty()) {
            StartupPackRun& last = runs.back();
            const bool adjacent = last.layer_idx == item.layer_idx &&
                                  last.start_block + last.len_blocks == item.block;
            if (adjacent && last.len_blocks + item.len <= kRunBlocks) {
                last.len_blocks += item.len;
                // The run inherits the *earliest* rank among its blocks: a
                // merged run is needed as early as its earliest member, so
                // taking the latest would delay a boot-critical block that
                // happened to merge with a later one.
                if (item.rank < last.first_need_rank) last.first_need_rank = item.rank;
                continue;
            }
        }
        StartupPackRun run;
        run.layer_idx = item.layer_idx;
        run.start_block = item.block;
        run.len_blocks = item.len;
        run.first_need_rank = item.rank;
        runs.push_back(run);
    }

    // Back to need order. Same runs and request count as physical order, but
    // the guest's first faults are satisfied first.
    std::stable_sort(runs.begin(), runs.end(), ByRank);
    return runs;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

namespace {

/// Rust `execute` — read and authenticate the manifest body.
core::Expected<std::vector<uint8_t>, std::string> ReadManifestBody(
    const std::shared_ptr<StartupPackTask>& task) {
    if (task->PackSource() == 0) {
        return core::make_unexpected(std::string("startup pack has no source"));
    }
    if (task->PackSize() == 0) {
        return core::make_unexpected(std::string("startup pack is empty"));
    }

    // The magic first, from a tiny read: a legacy or corrupt startup object
    // is rejected without downloading the whole thing.
    const size_t probe_len =
        static_cast<size_t>(task->PackSize() < 64 ? task->PackSize() : 64);
    const core::Expected<ByteBuffer, std::string> probe =
        task->PackSource()->ReadAt(0, probe_len);
    if (!probe.ok()) {
        return core::make_unexpected("read manifest magic probe: " + probe.error());
    }
    task->AddStats(1, probe.value().size());

    const size_t magic_len = sizeof(startup::kManifestMagic);
    if (probe.value().size() < magic_len ||
        std::memcmp(&probe.value()[0], startup::kManifestMagic, magic_len) != 0) {
        return core::make_unexpected(
            std::string("unsupported startup pack format (not a startup manifest)"));
    }

    std::vector<uint8_t> body = probe.value();
    if (task->PackSize() > probe_len) {
        const core::Expected<ByteBuffer, std::string> rest = task->PackSource()->ReadAt(
            probe_len, static_cast<size_t>(task->PackSize()) - probe_len);
        if (!rest.ok()) return core::make_unexpected("read manifest body: " + rest.error());
        task->AddStats(1, rest.value().size());
        body.insert(body.end(), rest.value().begin(), rest.value().end());
    }

    if (static_cast<uint64_t>(body.size()) != task->PackSize()) {
        std::ostringstream o;
        o << "short manifest: expect " << task->PackSize() << ", got " << body.size();
        return core::make_unexpected(o.str());
    }
    return body;
}

/// Rust's trust gate in `execute_manifest_prefetch`.
core::Expected<startup::StartupManifest, std::string> DecodeTrustedManifest(
    const std::shared_ptr<StartupPackTask>& task, const std::vector<uint8_t>& body) {
    // The committed record's digest has to match the body before a single
    // entry is acted on. Without this, a swapped manifest would redirect the
    // prefetch at arbitrary offsets of the layer objects.
    const std::string body_sha256 = core::Sha256Hex(body.empty() ? 0 : &body[0], body.size());
    if (body_sha256 != task->IndexSha256()) {
        return core::make_unexpected(
            std::string("manifest sha256 mismatch with the committed descriptor"));
    }

    const core::Expected<startup::StartupManifest, std::string> manifest =
        startup::DecodeManifest(body.empty() ? 0 : &body[0], body.size());
    if (!manifest.ok()) return core::make_unexpected("decode manifest: " + manifest.error());

    // Checked against the descriptor as well as decoded: the manifest's page
    // offsets only mean anything against the image size they were recorded
    // for, and planning them against a different one would resolve to the
    // wrong blocks.
    if (manifest.value().mem_virtual_size != task->MemVirtualSize()) {
        std::ostringstream o;
        o << "manifest mem size mismatch: descriptor " << task->MemVirtualSize()
          << " vs header " << manifest.value().mem_virtual_size;
        return core::make_unexpected(o.str());
    }
    return manifest.value();
}

}  // namespace

core::Expected<core::Unit, std::string> RunStartupPackTask(
    const std::shared_ptr<StartupPackTask>& task, const std::atomic<bool>* running,
    uint32_t block_slots_cap) {
    if (!task) return core::make_unexpected(std::string("null startup pack task"));

    task->SetPhase(kStartupPackRunning);

    // One helper so every exit path lands in a terminal phase. Rust does this
    // in `run_startup_pack_task` by matching on the outcome; the distinction
    // between Failed and Canceled is kept because a cancelled prefetch is a
    // normal lifecycle event and a failed one is not.
    struct Finish {
        const std::shared_ptr<StartupPackTask>& task;
        bool ok;
        ~Finish() {
            if (ok) {
                task->SetPhase(kStartupPackDone);
            } else if (task->IsCanceled()) {
                task->SetPhase(kStartupPackCanceled);
            } else {
                task->SetPhase(kStartupPackFailed);
            }
        }
    };
    Finish finish = {task, false};

    if (task->Layers().empty()) {
        return core::make_unexpected(std::string("manifest prefetch without bound layers"));
    }

    // Refused rather than silently mis-refilled. The planner's block ids are
    // in `kObjectBlockBytes` units and the refill reads them in the cache's
    // block size; if those disagree the prefetch would fetch the wrong
    // offsets and report success, which is worse than not prefetching.
    for (size_t i = 0; i < task->Layers().size(); ++i) {
        const StartupPackLayer& layer = task->Layers()[i];
        if (!layer.file) continue;
        const uint64_t block_size = layer.file->BackgroundBlockSize();
        if (!StartupPrefetchBlockSizeMatches(block_size)) {
            std::ostringstream o;
            o << "startup prefetch requires a cache block size of " << kObjectBlockBytes
              << ", got " << block_size;
            return core::make_unexpected(o.str());
        }
    }

    const core::Expected<std::vector<uint8_t>, std::string> body = ReadManifestBody(task);
    if (!body.ok()) return core::make_unexpected(body.error());

    const core::Expected<startup::StartupManifest, std::string> manifest =
        DecodeTrustedManifest(task, body.value());
    if (!manifest.ok()) return core::make_unexpected(manifest.error());

    // Prefix pages first, then the rank-sorted ranges: the manifest's table
    // order *is* the first-need order, so feeding the planner in table order
    // is what makes the plan's positions mean "needed this early".
    std::vector<std::pair<uint64_t, uint64_t> > spans;
    spans.reserve(manifest.value().prefix_pages.size() + manifest.value().ranges.size());
    for (size_t i = 0; i < manifest.value().prefix_pages.size(); ++i) {
        spans.push_back(std::make_pair(manifest.value().prefix_pages[i], kPlanPageBytes));
    }
    for (size_t i = 0; i < manifest.value().ranges.size(); ++i) {
        spans.push_back(manifest.value().ranges[i]);
    }

    std::vector<FinalLayerSource> layers;
    for (size_t i = 0; i < task->Layers().size(); ++i) {
        const StartupPackLayer& layer = task->Layers()[i];
        FinalLayerSource source;
        source.digest = layer.digest;
        source.size = layer.size;
        // Planned through the *cached* handle, so metadata reads the planner
        // makes are themselves cached rather than refetched per layer.
        source.source = FinalLayerBytes::OfVFile(layer.file);
        layers.push_back(source);
    }

    // No budget: the manifest was already produced under one, so bounding it
    // again here could silently drop entries the producer considered
    // essential.
    const core::Expected<PackPlan, std::string> plan = PlanRanges(
        spans, manifest.value().mem_virtual_size, layers, 0xFFFFFFFFFFFFFFFFull);
    if (!plan.ok()) return core::make_unexpected("plan manifest blocks: " + plan.error());

    const std::vector<StartupPackRun> work = MergeRunsByFirstNeed(plan.value());
    if (work.empty()) {
        finish.ok = true;
        return core::Unit();
    }

    // Rust spawns `MANIFEST_WORKERS` tasks pulling from a shared cursor.
    // Threads here, bounded the same way, and additionally by the scheduler's
    // block budget: the cap exists to bound total scratch memory, which is
    // per in-flight run regardless of who issued it.
    size_t worker_count = kManifestWorkers;
    if (worker_count > work.size()) worker_count = work.size();
    if (block_slots_cap > 0 && worker_count > block_slots_cap) {
        worker_count = block_slots_cap;
    }
    if (worker_count == 0) worker_count = 1;

    std::atomic<size_t> cursor(0);
    std::mutex error_mu;
    std::string first_error;

    std::vector<std::thread> workers;
    for (size_t w = 0; w < worker_count; ++w) {
        workers.push_back(std::thread([&]() {
            for (;;) {
                if (running != 0 && !running->load()) {
                    std::lock_guard<std::mutex> lk(error_mu);
                    if (first_error.empty()) first_error = "scheduler shutting down";
                    return;
                }
                if (task->IsCanceled()) {
                    std::lock_guard<std::mutex> lk(error_mu);
                    if (first_error.empty()) first_error = "manifest prefetch canceled";
                    return;
                }
                // Checked per run, not once: the deadline covers the whole
                // prefetch, and a slow source must not run past it.
                if (task->DeadlinePassed()) {
                    std::lock_guard<std::mutex> lk(error_mu);
                    if (first_error.empty()) first_error = "manifest prefetch timed out";
                    return;
                }

                const size_t index = cursor.fetch_add(1);
                if (index >= work.size()) return;
                const StartupPackRun& run = work[index];
                if (run.layer_idx >= task->Layers().size()) continue;
                const StartupPackLayer& layer = task->Layers()[run.layer_idx];
                if (!layer.file || !layer.source) continue;

                // The startup floor, not the background one: this fetches the
                // working set the guest is about to fault on, so it is
                // admitted well past the steady-state trickle.
                const std::shared_ptr<BkReadPermit> permit =
                    DownloadGate::GateStartupBlockRead(running);
                if (!permit) {
                    std::lock_guard<std::mutex> lk(error_mu);
                    if (first_error.empty()) {
                        first_error = "manifest prefetch canceled before chunk dispatch";
                    }
                    return;
                }

                const core::Expected<uint64_t, std::string> bytes =
                    layer.file->BackgroundRefillRange(layer.source, layer.size,
                                                      run.start_block, run.len_blocks);
                if (!bytes.ok()) {
                    std::lock_guard<std::mutex> lk(error_mu);
                    if (first_error.empty()) first_error = bytes.error();
                    return;
                }
                task->AddStats(1, bytes.value());
            }
        }));
    }
    for (size_t w = 0; w < workers.size(); ++w) workers[w].join();

    if (!first_error.empty()) return core::make_unexpected(first_error);
    finish.ok = true;
    return core::Unit();
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
