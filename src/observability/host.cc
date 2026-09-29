// SPDX-License-Identifier: MIT
// Rust: src/observability/host.rs
#include "agentenv/observability/host.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include <sys/statvfs.h>

#include "agentenv/core/logging.h"

namespace agentenv {
namespace observability {

const char* const kCgroupRoot     = "/sys/fs/cgroup";
const char* const kProcSelfCgroup = "/proc/self/cgroup";

// ---- helpers ---------------------------------------------------------------

static std::string ReadFileOrEmpty(const std::string& path) {
    std::ifstream f(path.c_str());
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- free functions --------------------------------------------------------

std::string DecodeMountField(const std::string& raw) {
    std::string buf;
    buf.reserve(raw.size());
    const char* p = raw.c_str();
    const char* end = p + raw.size();
    while (p < end) {
        if (*p == '\\' && (p + 3) < end &&
            (*(p+1) >= '0' && *(p+1) <= '7') &&
            (*(p+2) >= '0' && *(p+2) <= '7') &&
            (*(p+3) >= '0' && *(p+3) <= '7')) {
            unsigned char val = static_cast<unsigned char>(
                ((*(p+1) - '0') << 6) |
                ((*(p+2) - '0') << 3) |
                ((*(p+3) - '0')));
            buf.push_back(static_cast<char>(val));
            p += 4;
        } else {
            buf.push_back(*p++);
        }
    }
    return buf;
}

std::vector<MountEntry> ParseMounts(const std::string& content) {
    std::vector<MountEntry> result;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string device, mount_point, fs_type;
        if (!(ls >> device >> mount_point >> fs_type)) continue;
        MountEntry e;
        e.device          = DecodeMountField(device);
        e.mount_point     = DecodeMountField(mount_point);
        e.filesystem_type = fs_type;
        result.push_back(e);
    }
    return result;
}

std::string CgroupPathJoin(const std::string& base,
                           const std::string& cgroup_path,
                           const std::string& file_name) {
    std::string rel = cgroup_path;
    // trim leading '/'
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty()) {
        return base + "/" + file_name;
    }
    return base + "/" + rel + "/" + file_name;
}

std::vector<std::string> CgroupFileCandidates(const std::string& proc_self_cgroup,
                                              const std::string& cgroup_root,
                                              const std::string& file_name) {
    std::vector<std::string> candidates;
    std::istringstream ss(proc_self_cgroup);
    std::string line;
    while (std::getline(ss, line)) {
        // format: hierarchy_id:controllers:cgroup_path
        std::string::size_type c1 = line.find(':');
        if (c1 == std::string::npos) continue;
        std::string::size_type c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        std::string controllers = line.substr(c1 + 1, c2 - c1 - 1);
        std::string cgroup_path = line.substr(c2 + 1);
        // Rust only emits a candidate for cgroup-v2 rows (empty controller list).
        if (!controllers.empty()) continue;
        candidates.push_back(CgroupPathJoin(cgroup_root, cgroup_path, file_name));
    }
    return candidates;
}

uint32_t CpuPercentBetween(const CpuSample& previous, const CpuSample& current) {
    uint64_t delta_total = current.total > previous.total
                           ? current.total - previous.total : 0;
    if (delta_total == 0) return 0;
    uint64_t delta_idle = current.idle > previous.idle
                          ? current.idle - previous.idle : 0;
    uint64_t busy = delta_total > delta_idle ? delta_total - delta_idle : 0;
    return static_cast<uint32_t>((static_cast<double>(busy) / delta_total) * 100.0);
}

// ---- CgroupFilePaths -------------------------------------------------------

CgroupFilePaths CgroupFilePaths::Detect() {
    std::string content = ReadFileOrEmpty(kProcSelfCgroup);
    return FromProcSelfCgroup(content, kCgroupRoot);
}

CgroupFilePaths CgroupFilePaths::FromProcSelfCgroup(const std::string& content,
                                                    const std::string& cgroup_root) {
    CgroupFilePaths r;
    r.cpu_max        = CgroupFileCandidates(content, cgroup_root, "cpu.max");
    r.memory_max     = CgroupFileCandidates(content, cgroup_root, "memory.max");
    r.memory_current = CgroupFileCandidates(content, cgroup_root, "memory.current");
    return r;
}

core::Optional<std::string> CgroupFilePaths::ReadFirst(
    const std::vector<std::string>& paths) const {
    for (const auto& path : paths) {
        std::ifstream f(path.c_str());
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    return core::nullopt;
}

// ---- HostMetricsCollector static helpers -----------------------------------

core::Expected<CpuSample, std::string> HostMetricsCollector::ReadCpuSample() {
    std::string content = ReadFileOrEmpty("/proc/stat");
    if (content.empty()) {
        return core::make_unexpected(std::string("read /proc/stat failed"));
    }
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() < 4 || line.substr(0, 4) != "cpu ") continue;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        std::vector<uint64_t> vals;
        uint64_t v;
        while (ls >> v) vals.push_back(v);
        if (vals.size() < 5) {
            return core::make_unexpected(
                std::string("parse CPU counters from /proc/stat"));
        }
        // idle = idle + iowait (indices 3, 4)
        uint64_t idle = vals[3] + vals[4];
        uint64_t total = 0;
        for (uint64_t x : vals) total += x;
        CpuSample s;
        s.total = total;
        s.idle  = idle;
        return s;
    }
    return core::make_unexpected(
        std::string("missing aggregate cpu line in /proc/stat"));
}

core::Expected<std::pair<uint64_t, uint64_t>, std::string>
HostMetricsCollector::ReadMemoryBytes() {
    std::string content = ReadFileOrEmpty("/proc/meminfo");
    if (content.empty()) {
        return core::make_unexpected(std::string("read /proc/meminfo failed"));
    }
    uint64_t total_kib = 0, available_kib = 0;
    bool has_total = false, has_avail = false;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::string::size_type colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        std::istringstream vs(value);
        uint64_t kib;
        if (!(vs >> kib)) continue;
        if (key == "MemTotal")     { total_kib = kib;     has_total = true; }
        if (key == "MemAvailable") { available_kib = kib; has_avail = true; }
    }
    if (!has_total) {
        return core::make_unexpected(std::string("MemTotal missing in /proc/meminfo"));
    }
    if (!has_avail) {
        return core::make_unexpected(
            std::string("MemAvailable missing in /proc/meminfo"));
    }
    uint64_t total = total_kib * 1024;
    uint64_t avail = available_kib * 1024;
    uint64_t used  = total > avail ? total - avail : 0;
    return std::make_pair(total, used);
}

// Rust `is_real_mount`
bool HostMetricsCollector::IsRealMount(const MountEntry& m) {
    const std::string& fs = m.filesystem_type;
    bool good_fs = (fs == "ext2" || fs == "ext3" || fs == "ext4" ||
                    fs == "xfs"  || fs == "btrfs" || fs == "overlay" ||
                    fs == "tmpfs");
    if (!good_fs) return false;
    return (!m.device.empty() && m.device[0] == '/') || fs == "tmpfs";
}

core::Expected<std::vector<DiskMetric>, std::string>
HostMetricsCollector::ReadDiskMetrics() {
    std::string content = ReadFileOrEmpty("/proc/self/mounts");
    if (content.empty()) {
        return core::make_unexpected(std::string("read /proc/self/mounts failed"));
    }
    std::vector<MountEntry> mounts = ParseMounts(content);
    std::unordered_set<std::string> seen;
    std::vector<DiskMetric> disks;
    for (const auto& m : mounts) {
        if (!seen.insert(m.mount_point).second) continue;
        if (!IsRealMount(m)) continue;
        struct ::statvfs st;
        if (::statvfs(m.mount_point.c_str(), &st) != 0) continue;
        uint64_t bs    = static_cast<uint64_t>(st.f_frsize);
        uint64_t total = static_cast<uint64_t>(st.f_blocks) * bs;
        uint64_t free_ = static_cast<uint64_t>(st.f_bfree)  * bs;
        DiskMetric d;
        d.mount_point     = m.mount_point;
        d.device          = m.device;
        d.filesystem_type = m.filesystem_type;
        d.total_bytes     = total;
        d.used_bytes      = total > free_ ? total - free_ : 0;
        disks.push_back(d);
    }
    return disks;
}

core::Optional<uint32_t> HostMetricsCollector::ParseCgroupV2CpuLimit(
    const std::string& content) {
    std::istringstream ss(content);
    std::string quota_str, period_str;
    ss >> quota_str >> period_str;
    if (quota_str == "max") return core::nullopt;
    uint64_t quota = 0, period = 0;
    try {
        quota  = std::stoull(quota_str);
        period = std::stoull(period_str);
    } catch (...) {
        return core::nullopt;
    }
    return CpuQuotaToCount(quota, period);
}

core::Optional<uint32_t> HostMetricsCollector::CpuQuotaToCount(uint64_t quota,
                                                               uint64_t period) {
    if (quota == 0 || period == 0) return core::nullopt;
    uint64_t count = quota / period;
    if (count == 0) count = 1;  // sub-CPU limit: at least 1
    if (count > UINT32_MAX) count = UINT32_MAX;
    return static_cast<uint32_t>(count);
}

core::Optional<uint64_t> HostMetricsCollector::ParseCgroupMemoryLimit(
    const std::string& content) {
    std::string val = content;
    // trim
    while (!val.empty() && (val.back() == '\n' || val.back() == '\r' ||
                             val.back() == ' '))
        val.pop_back();
    if (val == "max") return core::nullopt;
    try {
        uint64_t v = std::stoull(val);
        if (v == 0) return core::nullopt;
        return v;
    } catch (...) {
        return core::nullopt;
    }
}

core::Optional<uint64_t> HostMetricsCollector::ParseCgroupMemoryUsage(
    const std::string& content) {
    std::string val = content;
    while (!val.empty() && (val.back() == '\n' || val.back() == '\r' ||
                             val.back() == ' '))
        val.pop_back();
    try {
        return std::stoull(val);
    } catch (...) {
        return core::nullopt;
    }
}

// ---- HostMetricsCollector --------------------------------------------------

HostMetricsCollector::HostMetricsCollector()
    : previous_cpu_(new CpuBaseline()),
      cgroup_files_(new CgroupFilePaths(CgroupFilePaths::Detect())) {}

core::Optional<uint32_t> HostMetricsCollector::ReadCgroupCpuLimitCount() const {
    core::Optional<std::string> raw = cgroup_files_->ReadFirst(cgroup_files_->cpu_max);
    if (!raw) return core::nullopt;
    return ParseCgroupV2CpuLimit(*raw);
}

core::Optional<CgroupMemoryLimit> HostMetricsCollector::ReadCgroupMemoryLimit() const {
    core::Optional<std::string> limit_raw =
        cgroup_files_->ReadFirst(cgroup_files_->memory_max);
    if (!limit_raw) return core::nullopt;
    core::Optional<uint64_t> limit_bytes = ParseCgroupMemoryLimit(*limit_raw);
    if (!limit_bytes) return core::nullopt;
    CgroupMemoryLimit r;
    r.limit_bytes = *limit_bytes;
    core::Optional<std::string> usage_raw =
        cgroup_files_->ReadFirst(cgroup_files_->memory_current);
    if (usage_raw) r.used_bytes = ParseCgroupMemoryUsage(*usage_raw);
    return r;
}

void HostMetricsCollector::ResetCpuBaseline() const {
    core::Expected<CpuSample, std::string> s = ReadCpuSample();
    std::lock_guard<std::mutex> lk(previous_cpu_->mu);
    previous_cpu_->sample = s.ok() ? core::Optional<CpuSample>(s.value())
                                   : core::Optional<CpuSample>(core::nullopt);
}

core::Expected<HostMetrics, std::string>
HostMetricsCollector::CollectHostMetrics() const {
    // Read or initialise the CPU baseline.
    core::Optional<CpuSample> baseline;
    {
        std::lock_guard<std::mutex> lk(previous_cpu_->mu);
        baseline = previous_cpu_->sample;
    }

    CpuSample current_cpu;
    if (baseline) {
        core::Expected<CpuSample, std::string> cur = ReadCpuSample();
        if (!cur.ok()) return core::make_unexpected(cur.error());
        current_cpu = cur.value();
    } else {
        // First call: take two samples separated by the warm-up window.
        core::Expected<CpuSample, std::string> first = ReadCpuSample();
        if (!first.ok()) return core::make_unexpected(first.error());
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kFirstRequestCpuSampleWindowMs));
        core::Expected<CpuSample, std::string> second = ReadCpuSample();
        if (!second.ok()) return core::make_unexpected(second.error());
        baseline    = first.value();
        current_cpu = second.value();
    }

    uint32_t cpu_percent = CpuPercentBetween(*baseline, current_cpu);

    {
        std::lock_guard<std::mutex> lk(previous_cpu_->mu);
        previous_cpu_->sample = current_cpu;
    }

    uint32_t host_cpu_count =
        std::max(1u, static_cast<uint32_t>(std::thread::hardware_concurrency()));
    core::Optional<uint32_t> cgroup_limit = ReadCgroupCpuLimitCount();
    uint32_t cpu_count = host_cpu_count;
    if (cgroup_limit) {
        cpu_count = std::max(1u, std::min(*cgroup_limit, host_cpu_count));
    }

    core::Expected<std::pair<uint64_t, uint64_t>, std::string> mem = ReadMemoryBytes();
    if (!mem.ok()) return core::make_unexpected(mem.error());
    uint64_t host_mem_total = mem.value().first;
    uint64_t host_mem_used  = mem.value().second;

    uint64_t memory_total_bytes = host_mem_total;
    uint64_t memory_used_bytes  = host_mem_used;
    core::Optional<CgroupMemoryLimit> cgroup_mem = ReadCgroupMemoryLimit();
    if (cgroup_mem && cgroup_mem->limit_bytes <= host_mem_total) {
        memory_total_bytes = cgroup_mem->limit_bytes;
        uint64_t cgroup_used = cgroup_mem->used_bytes
                               ? *cgroup_mem->used_bytes
                               : host_mem_used;
        memory_used_bytes = std::min(cgroup_used, cgroup_mem->limit_bytes);
    }

    core::Expected<std::vector<DiskMetric>, std::string> disks = ReadDiskMetrics();
    if (!disks.ok()) return core::make_unexpected(disks.error());

    HostMetrics m;
    m.cpu_percent        = cpu_percent;
    m.cpu_count          = cpu_count;
    m.memory_total_bytes = memory_total_bytes;
    m.memory_used_bytes  = memory_used_bytes;
    m.disks              = disks.value();
    return m;
}

HostMetrics HostMetricsCollector::Collect() const {
    core::Expected<HostMetrics, std::string> result = CollectHostMetrics();
    if (!result.ok()) {
        // Rust: `warn!(error = %err, "failed to collect host metrics")`
        // then reset_cpu_baseline.
        ResetCpuBaseline();
        return HostMetrics();
    }
    return result.value();
}

}  // namespace observability
}  // namespace agentenv
