// SPDX-License-Identifier: MIT
// Mirrors the #[cfg(test)] blocks in:
//   src/observability/host.rs
//   src/observability/machine.rs
#include "microtest.h"

#include "agentenv/observability/host.h"
#include "agentenv/observability/machine.h"

using namespace agentenv::observability;
using namespace agentenv::core;

// ============================================================
// host.rs :: cpu_percent_between_uses_busy_delta
// ============================================================
MT_TEST(cpu_percent_between_uses_busy_delta) {
    CpuSample previous; previous.total = 100; previous.idle = 20;
    CpuSample current;  current.total  = 180; current.idle  = 40;
    // busy delta = (180-100) - (40-20) = 80 - 20 = 60  => 60/80 * 100 = 75
    MT_EXPECT_EQ(CpuPercentBetween(previous, current), 75u);
}

// ============================================================
// host.rs :: parse_cgroup_v2_cpu_limit_reads_quota
// ============================================================
MT_TEST(parse_cgroup_v2_cpu_limit_quota_2) {
    Optional<uint32_t> r = HostMetricsCollector::ParseCgroupV2CpuLimit("250000 100000");
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_EQ(*r, 2u);
}

MT_TEST(parse_cgroup_v2_cpu_limit_sub_cpu_clamps_to_1) {
    Optional<uint32_t> r = HostMetricsCollector::ParseCgroupV2CpuLimit("50000 100000");
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_EQ(*r, 1u);
}

MT_TEST(parse_cgroup_v2_cpu_limit_max_yields_none) {
    Optional<uint32_t> r = HostMetricsCollector::ParseCgroupV2CpuLimit("max 100000");
    MT_EXPECT_TRUE(!r.has_value());
}

// ============================================================
// host.rs :: parse_cgroup_memory_limit_ignores_unlimited
// ============================================================
MT_TEST(parse_cgroup_memory_limit_numeric) {
    Optional<uint64_t> r =
        HostMetricsCollector::ParseCgroupMemoryLimit("1073741824");
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_EQ(*r, static_cast<uint64_t>(1073741824));
}

MT_TEST(parse_cgroup_memory_limit_max_yields_none) {
    Optional<uint64_t> r = HostMetricsCollector::ParseCgroupMemoryLimit("max");
    MT_EXPECT_TRUE(!r.has_value());
}

// ============================================================
// host.rs :: cgroup_file_candidates_resolves_v2_path_once
// ============================================================
MT_TEST(cgroup_file_candidates_resolves_v2_path) {
    std::vector<std::string> paths = CgroupFileCandidates(
        "0::/kubepods.slice/pod123/container456\n",
        "/sys/fs/cgroup",
        "cpu.max");
    MT_EXPECT_EQ(paths.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(paths[0],
        std::string("/sys/fs/cgroup/kubepods.slice/pod123/container456/cpu.max"));
}

// ============================================================
// host.rs :: cgroup_file_paths_detects_v2_files_from_cached_proc_cgroup_content
// ============================================================
MT_TEST(cgroup_file_paths_from_proc_self_cgroup) {
    CgroupFilePaths p = CgroupFilePaths::FromProcSelfCgroup(
        "0::/kubepods.slice/pod-a/container-a\n",
        "/sys/fs/cgroup");
    MT_EXPECT_EQ(p.cpu_max.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(p.cpu_max[0],
        std::string("/sys/fs/cgroup/kubepods.slice/pod-a/container-a/cpu.max"));
    MT_EXPECT_EQ(p.memory_current.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(p.memory_current[0],
        std::string("/sys/fs/cgroup/kubepods.slice/pod-a/container-a/memory.current"));
}

// ============================================================
// host.rs :: decode_mount_field_unescapes_spaces
// ============================================================
MT_TEST(decode_mount_field_unescapes_spaces) {
    MT_EXPECT_EQ(DecodeMountField("/var/lib/my\\040dir"), std::string("/var/lib/my dir"));
}

// ============================================================
// host.rs :: decode_mount_field_handles_multibyte_utf8_octal_escapes
// ============================================================
MT_TEST(decode_mount_field_multibyte_utf8) {
    // 'é' → UTF-8 0xC3 0xA9 → octal \303\251
    MT_EXPECT_EQ(DecodeMountField("/mnt/caf\\303\\251"), std::string("/mnt/caf\xc3\xa9"));
}

// ============================================================
// host.rs :: parse_mounts_extracts_device_mount_and_fs_type
// ============================================================
MT_TEST(parse_mounts_basic) {
    std::vector<MountEntry> mounts =
        ParseMounts("/dev/root / ext4 rw 0 0\nproc /proc proc rw 0 0\n");
    MT_EXPECT_EQ(mounts.size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(mounts[0].device,          std::string("/dev/root"));
    MT_EXPECT_EQ(mounts[0].mount_point,     std::string("/"));
    MT_EXPECT_EQ(mounts[0].filesystem_type, std::string("ext4"));
    MT_EXPECT_EQ(mounts[1].device,          std::string("proc"));
    MT_EXPECT_EQ(mounts[1].mount_point,     std::string("/proc"));
    MT_EXPECT_EQ(mounts[1].filesystem_type, std::string("proc"));
}

// ============================================================
// machine.rs :: cpuinfo_parser_returns_first_matching_key
// ============================================================
MT_TEST(first_cpuinfo_value_returns_first_match) {
    const std::string cpuinfo = "model name\t: Intel(R)\ncpu family\t: 6\n";

    std::vector<std::string> family_keys;
    family_keys.push_back("cpu family");
    Optional<std::string> family = FirstCpuinfoValue(cpuinfo, family_keys);
    MT_EXPECT_TRUE(family.has_value());
    MT_EXPECT_EQ(*family, std::string("6"));

    std::vector<std::string> name_keys;
    name_keys.push_back("model name");
    Optional<std::string> name = FirstCpuinfoValue(cpuinfo, name_keys);
    MT_EXPECT_TRUE(name.has_value());
    MT_EXPECT_EQ(*name, std::string("Intel(R)"));
}

MT_TEST(first_cpuinfo_value_missing_key_yields_none) {
    std::vector<std::string> keys;
    keys.push_back("nonexistent");
    Optional<std::string> r = FirstCpuinfoValue("model name\t: X\n", keys);
    MT_EXPECT_TRUE(!r.has_value());
}

MT_MAIN
