// SPDX-License-Identifier: MIT
// Rust: src/observability/host.rs — request-time host CPU/memory/disk sampling.
//
// Porting notes:
//   * `Arc<RwLock<Option<CpuSample>>>` becomes a `shared_ptr` to a small state
//     struct guarded by a `pthread_rwlock_t` wrapper; C++11 has no
//     `std::shared_mutex`, so a plain `std::mutex` is used instead. The Rust
//     code only ever does read-then-write under contention from independent
//     requests, so exclusive locking is behaviourally identical.
//   * `nix::sys::statvfs` maps onto `::statvfs(3)`.
//   * `std::thread::available_parallelism()` maps onto
//     `std::thread::hardware_concurrency()`, with the same 1 fallback.
#ifndef AGENTENV_OBSERVABILITY_HOST_H_
#define AGENTENV_OBSERVABILITY_HOST_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace observability {

/// Rust: `const FIRST_REQUEST_CPU_SAMPLE_WINDOW: Duration = 100ms`.
static const int kFirstRequestCpuSampleWindowMs = 100;
/// Rust: `const CGROUP_ROOT`.
extern const char* const kCgroupRoot;
/// Rust: `const PROC_SELF_CGROUP`.
extern const char* const kProcSelfCgroup;

/// Rust struct `DiskMetric`.
struct DiskMetric {
    std::string mount_point;
    std::string device;
    std::string filesystem_type;
    uint64_t    used_bytes  = 0;
    uint64_t    total_bytes = 0;

    bool operator==(const DiskMetric& o) const {
        return mount_point == o.mount_point && device == o.device &&
               filesystem_type == o.filesystem_type && used_bytes == o.used_bytes &&
               total_bytes == o.total_bytes;
    }
    bool operator!=(const DiskMetric& o) const { return !(*this == o); }
};

/// Rust struct `HostMetrics` (`#[derive(Default)]`).
struct HostMetrics {
    uint32_t cpu_percent        = 0;
    uint32_t cpu_count          = 0;
    uint64_t memory_used_bytes  = 0;
    uint64_t memory_total_bytes = 0;
    std::vector<DiskMetric> disks;
};

/// Rust struct `CpuSample` (private).
struct CpuSample {
    uint64_t total = 0;
    uint64_t idle  = 0;
};

/// Rust struct `CgroupMemoryLimit` (private).
struct CgroupMemoryLimit {
    uint64_t                 limit_bytes = 0;
    core::Optional<uint64_t> used_bytes;
};

/// Rust struct `MountEntry` (private) — one `/proc/self/mounts` row.
struct MountEntry {
    std::string device;
    std::string mount_point;
    std::string filesystem_type;

    bool operator==(const MountEntry& o) const {
        return device == o.device && mount_point == o.mount_point &&
               filesystem_type == o.filesystem_type;
    }
    bool operator!=(const MountEntry& o) const { return !(*this == o); }
};

/// Rust struct `CgroupFilePaths` (private) — resolved cgroup-v2 file candidates.
struct CgroupFilePaths {
    std::vector<std::string> cpu_max;
    std::vector<std::string> memory_max;
    std::vector<std::string> memory_current;

    /// Rust `CgroupFilePaths::detect` — reads `/proc/self/cgroup`, defaulting to
    /// an empty string (hence no candidates) when unreadable.
    static CgroupFilePaths Detect();

    /// Rust `CgroupFilePaths::from_proc_self_cgroup`.
    static CgroupFilePaths FromProcSelfCgroup(const std::string& content,
                                              const std::string& cgroup_root);

    /// Rust `CgroupFilePaths::read_first` — first path that reads successfully.
    core::Optional<std::string> ReadFirst(const std::vector<std::string>& paths) const;

    bool operator==(const CgroupFilePaths& o) const {
        return cpu_max == o.cpu_max && memory_max == o.memory_max &&
               memory_current == o.memory_current;
    }
};

/// Rust struct `HostMetricsCollector`.
///
/// `#[derive(Clone)]` on the Rust side shares the CPU baseline between clones
/// (it is behind an `Arc`); copying this class does the same, because both
/// members are `shared_ptr`.
class HostMetricsCollector {
 public:
    /// Rust `HostMetricsCollector::new` / `Default`.
    HostMetricsCollector();

    /// Rust `collect` — on failure logs a warning, resets the CPU baseline and
    /// returns `HostMetrics::default()`.
    HostMetrics Collect() const;

    // ---- pieces exposed for unit tests (Rust has them as private fns that
    // ---- its in-file `mod tests` can still reach) --------------------------

    /// Rust `parse_cgroup_v2_cpu_limit`.
    static core::Optional<uint32_t> ParseCgroupV2CpuLimit(const std::string& content);
    /// Rust `cpu_quota_to_count`.
    static core::Optional<uint32_t> CpuQuotaToCount(uint64_t quota, uint64_t period);
    /// Rust `parse_cgroup_memory_limit` — `max` and 0 both yield no limit.
    static core::Optional<uint64_t> ParseCgroupMemoryLimit(const std::string& content);
    /// Rust `parse_cgroup_memory_usage`.
    static core::Optional<uint64_t> ParseCgroupMemoryUsage(const std::string& content);
    /// Rust `read_cpu_sample`.
    static core::Expected<CpuSample, std::string> ReadCpuSample();
    /// Rust `read_memory_bytes` — returns (total, used).
    static core::Expected<std::pair<uint64_t, uint64_t>, std::string> ReadMemoryBytes();
    /// Rust `read_disk_metrics`.
    static core::Expected<std::vector<DiskMetric>, std::string> ReadDiskMetrics();
    /// Rust `is_real_mount`.
    static bool IsRealMount(const MountEntry& mount);

 private:
    /// Rust `collect_host_metrics`.
    core::Expected<HostMetrics, std::string> CollectHostMetrics() const;
    /// Rust `reset_cpu_baseline`.
    void ResetCpuBaseline() const;
    /// Rust `read_cgroup_cpu_limit_count`.
    core::Optional<uint32_t> ReadCgroupCpuLimitCount() const;
    /// Rust `read_cgroup_memory_limit`.
    core::Optional<CgroupMemoryLimit> ReadCgroupMemoryLimit() const;

    /// Rust `Arc<RwLock<Option<CpuSample>>>`.
    struct CpuBaseline {
        std::mutex               mu;
        core::Optional<CpuSample> sample;
    };
    std::shared_ptr<CpuBaseline>     previous_cpu_;
    std::shared_ptr<CgroupFilePaths> cgroup_files_;
};

// ---- free functions (Rust module-level private fns) ------------------------

/// Rust `cgroup_file_candidates` — only cgroup-v2 rows (empty controller list).
std::vector<std::string> CgroupFileCandidates(const std::string& proc_self_cgroup,
                                              const std::string& cgroup_root,
                                              const std::string& file_name);

/// Rust `cgroup_path_join`.
std::string CgroupPathJoin(const std::string& base, const std::string& cgroup_path,
                           const std::string& file_name);

/// Rust `cpu_percent_between` — truncated busy/total ratio, 0 when no delta.
uint32_t CpuPercentBetween(const CpuSample& previous, const CpuSample& current);

/// Rust `parse_mounts` — drops rows with fewer than 3 fields.
std::vector<MountEntry> ParseMounts(const std::string& content);

/// Rust `decode_mount_field` — undoes `\\NNN` octal escaping.
std::string DecodeMountField(const std::string& raw);

}  // namespace observability
}  // namespace agentenv
#endif  // AGENTENV_OBSERVABILITY_HOST_H_
