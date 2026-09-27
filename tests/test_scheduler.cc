// SPDX-License-Identifier: MIT
// Tests for services::scheduler — strategy, filter, binding store.
#include "microtest.h"

#include "services/scheduler/internal.h"

using namespace agentenv::services::scheduler;

static RichNode rn(const std::string& id) {
    RichNode n; n.node.id = id; n.node.endpoint = id + ":7000"; return n;
}

MT_TEST(round_robin_cycles) {
    RoundRobinStrategy s;
    std::vector<RichNode> nodes;
    nodes.push_back(rn("a"));
    nodes.push_back(rn("b"));
    nodes.push_back(rn("c"));
    auto p1 = s.Select(nodes, nullptr);
    auto p2 = s.Select(nodes, nullptr);
    auto p3 = s.Select(nodes, nullptr);
    auto p4 = s.Select(nodes, nullptr);
    MT_EXPECT_TRUE(p1.ok() && p2.ok() && p3.ok() && p4.ok());
    MT_EXPECT_TRUE(p1.value().node.id == "a");
    MT_EXPECT_TRUE(p2.value().node.id == "b");
    MT_EXPECT_TRUE(p3.value().node.id == "c");
    MT_EXPECT_TRUE(p4.value().node.id == "a");
}

MT_TEST(strategy_empty_errors) {
    RoundRobinStrategy s;
    std::vector<RichNode> empty;
    MT_EXPECT_TRUE(!s.Select(empty, nullptr).ok());
}

MT_TEST(new_strategy_factory) {
    MT_EXPECT_TRUE(NewStrategy("random")->Name() == "random");
    MT_EXPECT_TRUE(NewStrategy("round_robin")->Name() == "round_robin");
    MT_EXPECT_TRUE(NewStrategy("bogus")->Name() == "round_robin");
}

MT_TEST(filter_keeps_snapshotless_nodes) {
    std::vector<RichNode> nodes;
    nodes.push_back(rn("no-hb"));           // no snapshot
    RichNode busy = rn("busy");
    busy.has_snapshot = true;
    busy.snapshot.sandbox_count = 100;
    nodes.push_back(busy);

    NodeResourceLimit limit;
    limit.has_max_sandbox_count = true;
    limit.max_sandbox_count = 10;

    std::vector<RichNode> out = FilterByResourceLimit(nodes, &limit);
    // "no-hb" kept (no metrics), "busy" dropped (100 > 10).
    MT_EXPECT_EQ(static_cast<int>(out.size()), 1);
    MT_EXPECT_TRUE(out[0].node.id == "no-hb");
}

MT_TEST(filter_nil_limit_passthrough) {
    std::vector<RichNode> nodes;
    nodes.push_back(rn("a"));
    std::vector<RichNode> out = FilterByResourceLimit(nodes, nullptr);
    MT_EXPECT_EQ(static_cast<int>(out.size()), 1);
}

MT_TEST(binding_store_ttl) {
    InMemoryBindingStore store(1000);
    Node n; n.id = "node-x";
    store.Record("sb-1", n, 0);
    auto got = store.Get("sb-1", 500);
    MT_EXPECT_TRUE(got.ok());
    MT_EXPECT_TRUE(got.value().id == "node-x");
    // Expired at now=1000.
    auto exp = store.Get("sb-1", 1000);
    MT_EXPECT_TRUE(!exp.ok());
}

MT_MAIN
