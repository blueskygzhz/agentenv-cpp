// SPDX-License-Identifier: MIT
// Rust: src/api/impls/admin.rs
#include "agentenv/api/admin.h"

#include <string>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

class FakeObservability : public AdminObservability {
 public:
    bool        enabled    = true;
    bool        fail       = false;
    std::string node_id    = "node-1";
    std::string cluster_id = "11111111-2222-3333-4444-555555555555";
    observability::NodeSnapshot snapshot;

    FakeObservability() {
        snapshot.version               = "1.2.3";
        snapshot.commit                = "abcdef";
        snapshot.node_id               = "node-1";
        snapshot.service_instance_id   = "svc-1";
        snapshot.sandbox_count         = 3;
        snapshot.create_successes      = 10;
        snapshot.create_fails          = 1;
        snapshot.sandbox_starting_count = 2;
        snapshot.paused_sandbox_count  = 4;
        snapshot.machine_info.cpu_family       = "6";
        snapshot.machine_info.cpu_model        = "143";
        snapshot.machine_info.cpu_model_name   = "Xeon";
        snapshot.machine_info.cpu_architecture = "x86_64";
        snapshot.metrics.allocated_cpu      = 8;
        snapshot.metrics.cpu_percent        = 42;
        snapshot.metrics.cpu_count          = 64;
        snapshot.metrics.memory_used_bytes  = 1024;
        snapshot.metrics.memory_total_bytes = 4096;
        snapshot.metrics.paused_allocated_cpu          = 2;
        snapshot.metrics.paused_allocated_memory_bytes = 512;

        observability::DiskMetric disk;
        disk.mount_point     = "/";
        disk.device          = "/dev/sda1";
        disk.filesystem_type = "ext4";
        disk.used_bytes      = 100;
        disk.total_bytes     = 200;
        snapshot.metrics.disks.push_back(disk);
    }

    bool Enabled() const override { return enabled; }
    std::string NodeId() const override { return node_id; }
    std::string ClusterId() const override { return cluster_id; }
    core::Expected<observability::NodeSnapshot, std::string>
        NodeSnapshot() const override {
        if (fail) return core::make_unexpected(std::string("sampling failed"));
        return snapshot;
    }
};

int64_t IntField(const core::Json& json, const char* name) {
    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator it = fields.find(name);
    MT_EXPECT_TRUE(it != fields.end());
    return it->second.as_int();
}

std::string StringField(const core::Json& json, const char* name) {
    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator it = fields.find(name);
    MT_EXPECT_TRUE(it != fields.end());
    return it->second.as_string();
}

bool HasField(const core::Json& json, const char* name) {
    return json.as_object().find(name) != json.as_object().end();
}

}  // namespace

MT_TEST(admin_disk_metrics_projection) {
    observability::DiskMetric disk;
    disk.mount_point     = "/data";
    disk.device          = "/dev/nvme0n1";
    disk.filesystem_type = "xfs";
    disk.used_bytes      = 7;
    disk.total_bytes     = 9;

    const core::Json json = DiskMetricsToJson(disk);
    MT_EXPECT_EQ(StringField(json, "mountPoint"), std::string("/data"));
    MT_EXPECT_EQ(StringField(json, "device"), std::string("/dev/nvme0n1"));
    MT_EXPECT_EQ(StringField(json, "filesystemType"), std::string("xfs"));
    MT_EXPECT_EQ(IntField(json, "usedBytes"), static_cast<int64_t>(7));
    MT_EXPECT_EQ(IntField(json, "totalBytes"), static_cast<int64_t>(9));
}

MT_TEST(admin_machine_info_omits_cpu_config) {
    observability::MachineInfo info;
    info.cpu_family       = "6";
    info.cpu_model        = "143";
    info.cpu_model_name   = "Xeon";
    info.cpu_architecture = "x86_64";
    info.cpu_config_json  = core::Optional<std::string>("{\"secret\":true}");

    // Rust's `models::MachineInfo::new` takes only the four identity fields,
    // so the CPU template config is not part of the API shape.
    const core::Json json = MachineInfoToJson(info);
    MT_EXPECT_EQ(StringField(json, "cpuFamily"), std::string("6"));
    MT_EXPECT_TRUE(!HasField(json, "cpuConfigJson"));
    MT_EXPECT_TRUE(!HasField(json, "cpu_config_json"));
}

MT_TEST(admin_node_metrics_projection_includes_paused_reservations) {
    FakeObservability observability;
    const core::Json json = NodeMetricsToJson(observability.snapshot.metrics);

    MT_EXPECT_EQ(IntField(json, "allocatedCpu"), static_cast<int64_t>(8));
    MT_EXPECT_EQ(IntField(json, "cpuPercent"), static_cast<int64_t>(42));
    MT_EXPECT_EQ(IntField(json, "cpuCount"), static_cast<int64_t>(64));
    MT_EXPECT_EQ(IntField(json, "memoryTotalBytes"), static_cast<int64_t>(4096));
    // Paused reservations are reported separately: they are held but not in
    // use, which a scheduler needs to tell apart from live allocation.
    MT_EXPECT_EQ(IntField(json, "pausedAllocatedCpu"), static_cast<int64_t>(2));
    MT_EXPECT_EQ(IntField(json, "pausedAllocatedMemoryBytes"), static_cast<int64_t>(512));

    const core::JsonObject& fields = json.as_object();
    core::JsonObject::const_iterator disks = fields.find("disks");
    MT_EXPECT_TRUE(disks != fields.end());
    MT_EXPECT_EQ(disks->second.as_array().size(), static_cast<std::size_t>(1));
}

MT_TEST(admin_node_and_detail_differ_only_in_starting_count) {
    FakeObservability observability;
    const core::Json node   = NodeToJson(observability.snapshot);
    const core::Json detail = NodeDetailToJson(observability.snapshot);

    // Only the collection shape carries `sandboxStartingCount`; the
    // difference is preserved rather than smoothed over.
    MT_EXPECT_TRUE(HasField(node, "sandboxStartingCount"));
    MT_EXPECT_EQ(IntField(node, "sandboxStartingCount"), static_cast<int64_t>(2));
    MT_EXPECT_TRUE(!HasField(detail, "sandboxStartingCount"));

    // Everything else matches.
    MT_EXPECT_EQ(StringField(node, "nodeID"), StringField(detail, "nodeID"));
    MT_EXPECT_EQ(StringField(node, "status"), std::string("ready"));
    MT_EXPECT_EQ(IntField(node, "pausedSandboxCount"), static_cast<int64_t>(4));
    MT_EXPECT_EQ(IntField(detail, "pausedSandboxCount"), static_cast<int64_t>(4));
}

MT_TEST(admin_nodes_get_returns_single_node) {
    FakeObservability observability;
    auto nodes = NodesGet(&observability, core::Optional<std::string>());
    MT_EXPECT_TRUE(nodes.ok());
    MT_EXPECT_EQ(nodes.value().size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(StringField(nodes.value()[0], "version"), std::string("1.2.3"));
}

MT_TEST(admin_nodes_get_empty_when_observability_disabled) {
    FakeObservability observability;
    observability.enabled = false;

    // An empty list, not a synthetic record: to a scheduler reading the list a
    // partial node is indistinguishable from a real one.
    auto nodes = NodesGet(&observability, core::Optional<std::string>());
    MT_EXPECT_TRUE(nodes.ok());
    MT_EXPECT_TRUE(nodes.value().empty());

    auto null_service = NodesGet(NULL, core::Optional<std::string>());
    MT_EXPECT_TRUE(null_service.ok());
    MT_EXPECT_TRUE(null_service.value().empty());
}

MT_TEST(admin_nodes_get_filters_by_cluster) {
    FakeObservability observability;

    auto matching = NodesGet(&observability,
                             core::Optional<std::string>(observability.cluster_id));
    MT_EXPECT_TRUE(matching.ok());
    MT_EXPECT_EQ(matching.value().size(), static_cast<std::size_t>(1));

    auto other = NodesGet(&observability,
                          core::Optional<std::string>(std::string("other-cluster")));
    MT_EXPECT_TRUE(other.ok());
    MT_EXPECT_TRUE(other.value().empty());
}

MT_TEST(admin_nodes_get_reports_snapshot_failure) {
    FakeObservability observability;
    observability.fail = true;

    auto nodes = NodesGet(&observability, core::Optional<std::string>());
    MT_EXPECT_TRUE(!nodes.ok());
    MT_EXPECT_EQ(nodes.error().status, 500);
    MT_EXPECT_TRUE(nodes.error().message.find("sampling failed") != std::string::npos);
}

MT_TEST(admin_node_detail_returns_matching_node) {
    FakeObservability observability;
    auto detail =
        NodesNodeIdGet(&observability, "node-1", core::Optional<std::string>());
    MT_EXPECT_TRUE(detail.ok());
    MT_EXPECT_EQ(StringField(detail.value(), "nodeID"), std::string("node-1"));
    MT_EXPECT_EQ(StringField(detail.value(), "serviceInstanceID"), std::string("svc-1"));
}

MT_TEST(admin_node_detail_404_for_unknown_node) {
    FakeObservability observability;
    auto detail =
        NodesNodeIdGet(&observability, "node-2", core::Optional<std::string>());
    MT_EXPECT_TRUE(!detail.ok());
    MT_EXPECT_EQ(detail.error().status, 404);
    MT_EXPECT_TRUE(detail.error().message.find("node-2") != std::string::npos);
}

MT_TEST(admin_node_detail_404_on_cluster_mismatch) {
    FakeObservability observability;
    auto detail = NodesNodeIdGet(&observability, "node-1",
                                 core::Optional<std::string>(std::string("other")));
    MT_EXPECT_TRUE(!detail.ok());
    MT_EXPECT_EQ(detail.error().status, 404);
}

MT_TEST(admin_node_detail_404_when_observability_disabled) {
    FakeObservability observability;
    observability.enabled = false;

    // Rust reports this as "not found" rather than an error: node details are
    // simply unavailable on a process without observability.
    auto detail =
        NodesNodeIdGet(&observability, "node-1", core::Optional<std::string>());
    MT_EXPECT_TRUE(!detail.ok());
    MT_EXPECT_EQ(detail.error().status, 404);
    MT_EXPECT_TRUE(detail.error().message.find("disabled") != std::string::npos);

    auto null_service =
        NodesNodeIdGet(NULL, "node-1", core::Optional<std::string>());
    MT_EXPECT_TRUE(!null_service.ok());
    MT_EXPECT_EQ(null_service.error().status, 404);
}

MT_TEST(admin_node_detail_reports_snapshot_failure) {
    FakeObservability observability;
    observability.fail = true;

    auto detail =
        NodesNodeIdGet(&observability, "node-1", core::Optional<std::string>());
    MT_EXPECT_TRUE(!detail.ok());
    MT_EXPECT_EQ(detail.error().status, 500);
}

int main() { return microtest::RunAll(); }
