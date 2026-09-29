// SPDX-License-Identifier: MIT
// Rust: src/observability/reporter.rs `mod tests` + service.rs projection.
#include "agentenv/observability/reporter.h"

#include <string>

#include "agentenv/observability/service.h"
#include "microtest.h"

using namespace agentenv;              // NOLINT
using namespace agentenv::observability;  // NOLINT

namespace {

cfg::ClusterConfig MakeClusterConfig(const char* endpoint) {
    cfg::ClusterConfig c;
    if (endpoint) c.scheduler_endpoint = std::string(endpoint);
    return c;
}

cfg::ObservabilitySchedulerReportConfig MakeReportConfig(bool enabled, uint64_t interval_secs) {
    cfg::ObservabilitySchedulerReportConfig c;
    c.enabled = enabled;
    c.interval_secs = interval_secs;
    return c;
}

}  // namespace

// ---- Rust: test_resolve_returns_none_when_no_config ----
MT_TEST(resolve_returns_none_when_no_config) {
    // Rust's default for `enabled` is false.
    auto r = ReporterConfig::Resolve(MakeReportConfig(false, 5), MakeClusterConfig(NULL));
    MT_EXPECT_TRUE(!r.has_value());
}

// ---- Rust: test_resolve_returns_none_when_report_is_disabled ----
MT_TEST(resolve_returns_none_when_report_is_disabled) {
    auto r = ReporterConfig::Resolve(MakeReportConfig(false, 10),
                                     MakeClusterConfig("http://scheduler:9090"));
    MT_EXPECT_TRUE(!r.has_value());
}

// ---- Rust: test_resolve_returns_none_when_endpoint_is_blank ----
MT_TEST(resolve_returns_none_when_endpoint_is_blank) {
    auto r = ReporterConfig::Resolve(MakeReportConfig(true, 5), MakeClusterConfig("   "));
    MT_EXPECT_TRUE(!r.has_value());
    // A missing endpoint is equally disabling.
    auto missing = ReporterConfig::Resolve(MakeReportConfig(true, 5), MakeClusterConfig(NULL));
    MT_EXPECT_TRUE(!missing.has_value());
}

// ---- Rust: test_resolve_uses_config_values ----
MT_TEST(resolve_uses_config_values) {
    auto r = ReporterConfig::Resolve(MakeReportConfig(true, 10),
                                     MakeClusterConfig("http://scheduler:9090"));
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_TRUE(r->scheduler_endpoint == "http://scheduler:9090");
    MT_EXPECT_TRUE(r->interval_ms == 10 * 1000);
}

MT_TEST(resolve_trims_endpoint_whitespace) {
    auto r = ReporterConfig::Resolve(MakeReportConfig(true, 5),
                                     MakeClusterConfig("  http://scheduler:9090\t"));
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_TRUE(r->scheduler_endpoint == "http://scheduler:9090");
}

// ---- Rust: test_resolve_clamps_interval_to_minimum_one ----
MT_TEST(resolve_clamps_interval_to_minimum_one) {
    auto r = ReporterConfig::Resolve(MakeReportConfig(true, 0),
                                     MakeClusterConfig("http://scheduler:9090"));
    MT_EXPECT_TRUE(r.has_value());
    MT_EXPECT_TRUE(r->interval_ms == 1000);
}

// ---- Rust: HeartbeatNodeNotConfigured classification ----
MT_TEST(heartbeat_node_not_configured_classification) {
    const int kInvalidArgument = 3;
    const int kUnavailable = 14;
    MT_EXPECT_TRUE(IsHeartbeatNodeNotConfigured(
        kInvalidArgument, "node is not in scheduler node list: node-a"));
    // Right message, wrong code => generic transient error.
    MT_EXPECT_TRUE(!IsHeartbeatNodeNotConfigured(
        kUnavailable, "node is not in scheduler node list"));
    // Right code, unrelated message => generic transient error.
    MT_EXPECT_TRUE(!IsHeartbeatNodeNotConfigured(kInvalidArgument, "bad cluster id"));
    MT_EXPECT_TRUE(std::string(kHeartbeatNodeNotConfiguredMessage) ==
                   "node is not in the scheduler's configured node list");
}

// ---- Rust heartbeat loop: backoff doubles and saturates at MAX_REPORT_BACKOFF ----
MT_TEST(heartbeat_backoff_doubles_and_caps) {
    HeartbeatBackoff b(5000);  // interval = 5s
    // Rust seeds `wait` to 100ms so the first attempt is nearly immediate.
    MT_EXPECT_TRUE(b.wait_ms() == 100);
    MT_EXPECT_TRUE(b.backoff_ms() == 5000);

    MT_EXPECT_TRUE(b.OnFailure() == 5000);   // wait = backoff
    MT_EXPECT_TRUE(b.backoff_ms() == 10000);
    MT_EXPECT_TRUE(b.OnFailure() == 10000);
    MT_EXPECT_TRUE(b.backoff_ms() == 20000);
    MT_EXPECT_TRUE(b.OnFailure() == 20000);
    MT_EXPECT_TRUE(b.backoff_ms() == 40000);
    MT_EXPECT_TRUE(b.OnFailure() == 40000);
    // 80s would exceed the 60s cap.
    MT_EXPECT_TRUE(b.backoff_ms() == kMaxReportBackoffMs);
    MT_EXPECT_TRUE(b.OnFailure() == kMaxReportBackoffMs);
    MT_EXPECT_TRUE(b.backoff_ms() == kMaxReportBackoffMs);
}

MT_TEST(heartbeat_backoff_resets_on_success) {
    HeartbeatBackoff b(5000);
    b.OnFailure();
    b.OnFailure();
    MT_EXPECT_TRUE(b.backoff_ms() == 20000);
    b.OnSuccess();
    MT_EXPECT_TRUE(b.backoff_ms() == 5000);
    MT_EXPECT_TRUE(b.wait_ms() == 5000);
}

// ---- Rust shutdown rule: no successful heartbeat => skip UnregisterNode ----
MT_TEST(shutdown_skips_unregister_without_a_successful_heartbeat) {
    HeartbeatBackoff never(5000);
    never.OnFailure();
    MT_EXPECT_TRUE(!never.ever_succeeded());
    MT_EXPECT_TRUE(!never.ShouldUnregisterOnShutdown());

    HeartbeatBackoff once(5000);
    once.OnSuccess();
    once.OnFailure();  // a later failure must not clear the flag
    MT_EXPECT_TRUE(once.ever_succeeded());
    MT_EXPECT_TRUE(once.ShouldUnregisterOnShutdown());
}

MT_TEST(unregister_retry_schedule) {
    MT_EXPECT_TRUE(kUnregisterMaxAttempts == 3);
    MT_EXPECT_TRUE(UnregisterRetryDelayMs(1) == 200);
    MT_EXPECT_TRUE(UnregisterRetryDelayMs(2) == 400);
    MT_EXPECT_TRUE(UnregisterRetryDelayMs(3) == 600);
}

// ---- Rust: build_heartbeat_request field mapping ----
MT_TEST(build_heartbeat_request_maps_every_field) {
    NodeSnapshot snap;
    snap.version = "1.2.3";
    snap.commit = "abc123";
    snap.node_id = "node-a";
    snap.service_instance_id = "svc-1";
    snap.machine_info.cpu_family = "6";
    snap.machine_info.cpu_model = "143";
    snap.machine_info.cpu_model_name = "Xeon";
    snap.machine_info.cpu_architecture = "x86_64";
    snap.sandbox_count = 4;
    snap.create_successes = 11;
    snap.create_fails = 2;
    snap.sandbox_starting_count = 1;
    snap.paused_sandbox_count = 3;
    snap.metrics.allocated_cpu = 8;
    snap.metrics.allocated_memory_bytes = 4096;
    snap.metrics.cpu_percent = 55;
    snap.metrics.cpu_count = 16;
    snap.metrics.memory_used_bytes = 1024;
    snap.metrics.memory_total_bytes = 8192;
    snap.metrics.paused_allocated_cpu = 2;
    snap.metrics.paused_allocated_memory_bytes = 512;

    DiskMetric disk;
    disk.mount_point = "/";
    disk.device = "/dev/sda1";
    disk.filesystem_type = "ext4";
    disk.used_bytes = 100;
    disk.total_bytes = 200;
    snap.metrics.disks.push_back(disk);

    core::Optional<p2p::P2pEndpoint> no_endpoint;
    WireHeartbeatRequest req = BuildHeartbeatRequest(snap, 1700000000123LL, no_endpoint);

    MT_EXPECT_TRUE(req.node_id == "node-a");
    MT_EXPECT_TRUE(req.service_instance_id == "svc-1");
    MT_EXPECT_TRUE(req.version == "1.2.3");
    MT_EXPECT_TRUE(req.commit == "abc123");
    MT_EXPECT_TRUE(req.machine_info.cpu_model_name == "Xeon");
    // Rust `unwrap_or_default()` turns a missing CPU config into "".
    MT_EXPECT_TRUE(req.machine_info.cpu_config_json == "");
    // The node always reports itself Ready.
    MT_EXPECT_TRUE(req.snapshot.status == WireNodeStatus::Ready);
    MT_EXPECT_TRUE(req.snapshot.reported_at_unix_ms == 1700000000123LL);
    MT_EXPECT_TRUE(req.snapshot.allocated_cpu == 8);
    MT_EXPECT_TRUE(req.snapshot.allocated_memory_bytes == 4096);
    MT_EXPECT_TRUE(req.snapshot.cpu_percent == 55);
    MT_EXPECT_TRUE(req.snapshot.cpu_count == 16);
    MT_EXPECT_TRUE(req.snapshot.memory_used_bytes == 1024);
    MT_EXPECT_TRUE(req.snapshot.memory_total_bytes == 8192);
    MT_EXPECT_TRUE(req.snapshot.sandbox_count == 4);
    MT_EXPECT_TRUE(req.snapshot.sandbox_starting_count == 1);
    MT_EXPECT_TRUE(req.snapshot.create_successes == 11);
    MT_EXPECT_TRUE(req.snapshot.create_fails == 2);
    MT_EXPECT_TRUE(req.snapshot.paused_sandbox_count == 3);
    MT_EXPECT_TRUE(req.snapshot.paused_allocated_cpu == 2);
    MT_EXPECT_TRUE(req.snapshot.paused_allocated_memory_bytes == 512);
    MT_EXPECT_EQ(static_cast<int>(req.snapshot.disks.size()), 1);
    MT_EXPECT_TRUE(req.snapshot.disks[0].mount_point == "/");
    MT_EXPECT_TRUE(req.snapshot.disks[0].device == "/dev/sda1");
    MT_EXPECT_TRUE(req.snapshot.disks[0].filesystem_type == "ext4");
    MT_EXPECT_TRUE(req.snapshot.disks[0].used_bytes == 100);
    MT_EXPECT_TRUE(req.snapshot.disks[0].total_bytes == 200);
    MT_EXPECT_TRUE(!req.has_p2p_endpoint);
}

MT_TEST(build_heartbeat_request_carries_cpu_config_and_p2p_endpoint) {
    NodeSnapshot snap;
    snap.machine_info.cpu_config_json = std::string("{\"cpuid\":[]}");

    p2p::P2pEndpoint ep;
    ep.backend = "iroh";
    ep.address = "node-addr";
    core::Optional<p2p::P2pEndpoint> endpoint(ep);

    WireHeartbeatRequest req = BuildHeartbeatRequest(snap, 0, endpoint);
    MT_EXPECT_TRUE(req.machine_info.cpu_config_json == "{\"cpuid\":[]}");
    MT_EXPECT_TRUE(req.has_p2p_endpoint);
    MT_EXPECT_TRUE(req.p2p_endpoint.backend == "iroh");
    MT_EXPECT_TRUE(req.p2p_endpoint.address == "node-addr");
}

// ---- Rust: build_sandbox_event_request + map_sandbox_event_type ----
MT_TEST(build_sandbox_event_request_converts_mib_to_bytes) {
    std::vector<orchestrator::SandboxLifecycleEvent> events;
    orchestrator::SandboxLifecycleEvent ev;
    ev.event_type = orchestrator::SandboxLifecycleEventType::Create;
    ev.resources.cpu_count = 4;
    ev.resources.memory_mib = 2048;
    ev.resources.disk_size_mib = 8192;
    events.push_back(ev);

    WireReportSandboxEventRequest req =
        BuildSandboxEventRequest("node-a", "cluster-1", "svc-1", events);

    MT_EXPECT_TRUE(req.node_id == "node-a");
    MT_EXPECT_TRUE(req.cluster_id == "cluster-1");
    MT_EXPECT_TRUE(req.service_instance_id == "svc-1");
    MT_EXPECT_EQ(static_cast<int>(req.events.size()), 1);
    MT_EXPECT_TRUE(req.events[0].event_type == WireSandboxEventType::Create);
    MT_EXPECT_TRUE(req.events[0].requested_cpu == 4);
    // MiB -> bytes, widened before multiplying so 4 GiB does not overflow.
    MT_EXPECT_TRUE(req.events[0].requested_memory_bytes == 2048ULL * 1024 * 1024);
    MT_EXPECT_TRUE(req.events[0].requested_disk_bytes == 8192ULL * 1024 * 1024);
}

MT_TEST(build_sandbox_event_request_memory_does_not_overflow_u32) {
    std::vector<orchestrator::SandboxLifecycleEvent> events;
    orchestrator::SandboxLifecycleEvent ev;
    // 8 GiB in MiB: 8192 * 1024 * 1024 overflows u32 if not widened first.
    ev.resources.memory_mib = 8192;
    ev.resources.disk_size_mib = 1048576;  // 1 TiB
    events.push_back(ev);

    WireReportSandboxEventRequest req = BuildSandboxEventRequest("n", "c", "s", events);
    MT_EXPECT_TRUE(req.events[0].requested_memory_bytes == 8589934592ULL);
    MT_EXPECT_TRUE(req.events[0].requested_disk_bytes == 1099511627776ULL);
}

MT_TEST(map_sandbox_event_type_covers_all_variants) {
    MT_EXPECT_TRUE(MapSandboxEventType(orchestrator::SandboxLifecycleEventType::Create) ==
                   WireSandboxEventType::Create);
    MT_EXPECT_TRUE(MapSandboxEventType(orchestrator::SandboxLifecycleEventType::Delete) ==
                   WireSandboxEventType::Delete);
    MT_EXPECT_TRUE(MapSandboxEventType(orchestrator::SandboxLifecycleEventType::Pause) ==
                   WireSandboxEventType::Pause);
    MT_EXPECT_TRUE(MapSandboxEventType(orchestrator::SandboxLifecycleEventType::Resume) ==
                   WireSandboxEventType::Resume);
    MT_EXPECT_TRUE(MapSandboxEventType(orchestrator::SandboxLifecycleEventType::Fork) ==
                   WireSandboxEventType::Fork);
}

MT_TEST(build_sandbox_event_request_empty_batch) {
    // Rust short-circuits an empty batch before the RPC; the projection itself
    // still yields a well-formed request with no events.
    WireReportSandboxEventRequest req = BuildSandboxEventRequest(
        "n", "c", "s", std::vector<orchestrator::SandboxLifecycleEvent>());
    MT_EXPECT_TRUE(req.events.empty());
}

// ---- service.rs: node_snapshot projection ----
MT_TEST(project_node_snapshot_merges_all_sources) {
    core::NodeIdentity identity;
    identity.id = "node-a";
    identity.service_instance_id = "svc-1";
    identity.commit = "deadbeef";
    identity.version = "9.9.9";

    MachineInfo machine;
    machine.cpu_model_name = "Xeon";

    orchestrator::OrchestratorMetrics runtime;
    runtime.create_successes = 7;
    runtime.create_fails = 1;
    runtime.running_sandbox_count = 5;
    runtime.starting_sandbox_count = 2;
    runtime.allocated_cpu = 12;
    runtime.allocated_memory_bytes = 2048;
    runtime.paused_sandbox_count = 1;
    runtime.paused_allocated_cpu = 3;
    runtime.paused_allocated_memory_bytes = 256;

    HostMetrics host;
    host.cpu_percent = 42;
    host.cpu_count = 32;
    host.memory_used_bytes = 900;
    host.memory_total_bytes = 4096;

    std::vector<core::SandboxId> ids;
    ids.push_back(core::SandboxId::Fresh());

    NodeSnapshot snap = ProjectNodeSnapshot(identity, machine, runtime, host, ids);

    MT_EXPECT_TRUE(snap.node_id == "node-a");
    MT_EXPECT_TRUE(snap.version == "9.9.9");
    MT_EXPECT_TRUE(snap.commit == "deadbeef");
    MT_EXPECT_TRUE(snap.service_instance_id == "svc-1");
    MT_EXPECT_TRUE(snap.machine_info.cpu_model_name == "Xeon");
    // sandbox_count comes from the orchestrator's running count, not ids.len().
    MT_EXPECT_TRUE(snap.sandbox_count == 5);
    MT_EXPECT_EQ(static_cast<int>(snap.sandbox_ids.size()), 1);
    // Allocation from the orchestrator...
    MT_EXPECT_TRUE(snap.metrics.allocated_cpu == 12);
    MT_EXPECT_TRUE(snap.metrics.allocated_memory_bytes == 2048);
    MT_EXPECT_TRUE(snap.metrics.paused_allocated_cpu == 3);
    MT_EXPECT_TRUE(snap.metrics.paused_allocated_memory_bytes == 256);
    // ...utilization from the host.
    MT_EXPECT_TRUE(snap.metrics.cpu_percent == 42);
    MT_EXPECT_TRUE(snap.metrics.cpu_count == 32);
    MT_EXPECT_TRUE(snap.metrics.memory_used_bytes == 900);
    MT_EXPECT_TRUE(snap.metrics.memory_total_bytes == 4096);
    MT_EXPECT_TRUE(snap.create_successes == 7);
    MT_EXPECT_TRUE(snap.create_fails == 1);
    MT_EXPECT_TRUE(snap.sandbox_starting_count == 2);
    MT_EXPECT_TRUE(snap.paused_sandbox_count == 1);
}

// ---- service.rs: take_cpu_config_json is Option::take (once only) ----
MT_TEST(service_cpu_config_take_and_cluster_store) {
    ObservabilityService svc(
        core::NodeIdentity(),
        []() {
            return core::Expected<orchestrator::OrchestratorMetrics, std::string>(
                orchestrator::OrchestratorMetrics());
        },
        []() {
            return core::Expected<std::vector<core::SandboxId>, std::string>(
                std::vector<core::SandboxId>());
        },
        core::Optional<std::string>());

    // No cpu-template-helper configured => nothing pending.
    MT_EXPECT_TRUE(!svc.TakeCpuConfigJson().has_value());

    MT_EXPECT_TRUE(!svc.ClusterCpuConfig().has_value());
    svc.StoreClusterCpuConfig("{\"intersection\":1}");
    MT_EXPECT_TRUE(svc.ClusterCpuConfig().has_value());
    MT_EXPECT_TRUE(*svc.ClusterCpuConfig() == "{\"intersection\":1}");

    // The snapshot path works end-to-end off the injected samplers.
    auto snap = svc.NodeSnapshotNow();
    MT_EXPECT_TRUE(snap.ok());
}

MT_TEST(service_propagates_sampler_errors) {
    ObservabilityService svc(
        core::NodeIdentity(),
        []() {
            return core::Expected<orchestrator::OrchestratorMetrics, std::string>(
                core::make_unexpected(std::string("metrics unavailable")));
        },
        []() {
            return core::Expected<std::vector<core::SandboxId>, std::string>(
                std::vector<core::SandboxId>());
        },
        core::Optional<std::string>());

    auto snap = svc.NodeSnapshotNow();
    MT_EXPECT_TRUE(!snap.ok());
    MT_EXPECT_TRUE(snap.error() == "metrics unavailable");
}

MT_MAIN
