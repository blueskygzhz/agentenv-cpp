// SPDX-License-Identifier: MIT
// Tests for orchestrator::InMemoryMetadataStore + metrics aggregation.
#include "microtest.h"

#include "agentenv/orchestrator/metrics.h"
#include "agentenv/orchestrator/store.h"

using namespace agentenv;
using namespace agentenv::orchestrator;

static SandboxMetadata make_meta(LifecyclePhase state, uint32_t cpu, uint32_t mib) {
    SandboxMetadata m;
    m.id = core::SandboxId::Fresh();
    m.state = state;
    m.cpu_count = cpu;
    m.memory_mib = mib;
    return m;
}

MT_TEST(store_insert_get_update_remove) {
    InMemoryMetadataStore s;
    auto m = make_meta(LifecyclePhase::Running, 2, 256);
    core::SandboxId id = m.id;

    auto r1 = s.Insert(m);
    MT_EXPECT_TRUE(r1.ok());

    // Duplicate insert -> Conflict.
    auto r2 = s.Insert(m);
    MT_EXPECT_TRUE(!r2.ok());

    auto g = s.Get(id);
    MT_EXPECT_TRUE(g.ok());
    MT_EXPECT_TRUE(g.value().cpu_count == 2u);

    // Update.
    m.cpu_count = 8;
    MT_EXPECT_TRUE(s.Update(m).ok());
    MT_EXPECT_TRUE(s.Get(id).value().cpu_count == 8u);

    // Remove.
    MT_EXPECT_TRUE(s.Remove(id).ok());
    MT_EXPECT_TRUE(!s.Get(id).ok());
}

MT_TEST(metrics_aggregate_multiple_states) {
    InMemoryMetadataStore s;
    s.Insert(make_meta(LifecyclePhase::Running,  2, 256));
    s.Insert(make_meta(LifecyclePhase::Running,  1, 128));
    s.Insert(make_meta(LifecyclePhase::Booting,  4, 512));
    s.Insert(make_meta(LifecyclePhase::Stopped,  8, 1024));  // include_stopped=false by default

    SandboxListFilter f;
    auto list = s.List(f);
    MT_EXPECT_EQ(static_cast<int>(list.size()), 3);

    OrchestratorMetrics m;
    for (const SandboxMetadata& meta : list) {
        AggregateResourceMetrics(&m, SandboxContribution::FromMeta(meta));
    }
    MT_EXPECT_EQ(m.running_sandbox_count, 2u);
    MT_EXPECT_EQ(m.starting_sandbox_count, 1u);
    MT_EXPECT_EQ(m.allocated_cpu, 2u + 1u + 4u);
}

MT_TEST(counters_atomic_accumulate) {
    OrchestratorCounters c;
    c.RecordCreateSuccess(1);
    c.RecordCreateSuccess(1);
    c.RecordCreateFail(3);
    MT_EXPECT_EQ(c.CreateSuccesses(), 2ull);
    MT_EXPECT_EQ(c.CreateFails(), 3ull);
}

MT_MAIN
