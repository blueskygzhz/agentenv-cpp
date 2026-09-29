// SPDX-License-Identifier: MIT
// Mirrors the #[cfg(test)] blocks in:
//   src/orchestrator/metrics.rs
//   src/orchestrator/store/metadata.rs
#include "microtest.h"

#include "agentenv/orchestrator/metrics.h"
#include "agentenv/orchestrator/store.h"

using namespace agentenv;
using namespace agentenv::orchestrator;

// Rust test helper: `fn meta(state, cpu, memory_mib) -> SandboxMetadata`
static SandboxMetadata meta(SandboxState state, uint32_t cpu, uint32_t memory_mib) {
    SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = state;
    m.resources.cpu_count     = cpu;
    m.resources.memory_mib    = memory_mib;
    m.resources.disk_size_mib = 0;
    return m;
}

// Rust test helper: `fn aggregate(metas) -> OrchestratorMetrics`
static OrchestratorMetrics aggregate(const std::vector<SandboxMetadata>& metas) {
    OrchestratorMetrics metrics;
    for (std::size_t i = 0; i < metas.size(); ++i) {
        AggregateResourceMetrics(
            &metrics,
            SandboxContribution::New(metas[i].state, metas[i].resources));
    }
    return metrics;
}

// ---- metrics.rs :: counters_accumulate_monotonically ----
MT_TEST(counters_accumulate_monotonically) {
    OrchestratorCounters counters;
    counters.RecordCreateSuccess(1);
    counters.RecordCreateSuccess(1);
    counters.RecordCreateFail(1);
    counters.RecordCreateFail(3);
    MT_EXPECT_EQ(counters.CreateSuccesses(), static_cast<uint64_t>(2));
    MT_EXPECT_EQ(counters.CreateFails(),     static_cast<uint64_t>(4));
}

// ---- metrics.rs :: counters_accumulate_multiple_successes ----
MT_TEST(counters_accumulate_multiple_successes) {
    OrchestratorCounters counters;
    counters.RecordCreateSuccess(3);
    counters.RecordCreateFail(1);
    MT_EXPECT_EQ(counters.CreateSuccesses(), static_cast<uint64_t>(3));
    MT_EXPECT_EQ(counters.CreateFails(),     static_cast<uint64_t>(1));
}

// ---- metrics.rs :: aggregate_counts_running_and_starting ----
MT_TEST(aggregate_counts_running_and_starting) {
    std::vector<SandboxMetadata> metas;
    metas.push_back(meta(SandboxState::Running,  2, 256));
    metas.push_back(meta(SandboxState::Running,  1, 128));
    metas.push_back(meta(SandboxState::Creating, 4, 512));
    metas.push_back(meta(SandboxState::Resuming, 1, 64));

    OrchestratorMetrics m = aggregate(metas);
    MT_EXPECT_EQ(m.running_sandbox_count,  static_cast<uint32_t>(2));
    MT_EXPECT_EQ(m.starting_sandbox_count, static_cast<uint32_t>(2));
    MT_EXPECT_EQ(m.allocated_cpu, static_cast<uint32_t>(2 + 1 + 4 + 1));
    MT_EXPECT_EQ(m.allocated_memory_bytes,
                 static_cast<uint64_t>(256u + 128 + 512 + 64) * 1024 * 1024);
}

// ---- metrics.rs :: aggregate_excludes_only_paused_from_resources ----
// Paused is the only state that released its VM-side resources. Killing still
// holds CPU/memory and counts toward running_sandbox_count.
MT_TEST(aggregate_excludes_only_paused_from_resources) {
    std::vector<SandboxMetadata> metas;
    metas.push_back(meta(SandboxState::Paused,  8, 1024));
    metas.push_back(meta(SandboxState::Killing, 2, 256));
    metas.push_back(meta(SandboxState::Running, 1, 128));

    OrchestratorMetrics m = aggregate(metas);
    MT_EXPECT_EQ(m.running_sandbox_count,  static_cast<uint32_t>(2));
    MT_EXPECT_EQ(m.starting_sandbox_count, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.allocated_cpu,          static_cast<uint32_t>(2 + 1));
    MT_EXPECT_EQ(m.allocated_memory_bytes,
                 static_cast<uint64_t>(256u + 128) * 1024 * 1024);
    MT_EXPECT_EQ(m.paused_sandbox_count,   static_cast<uint32_t>(1));
    MT_EXPECT_EQ(m.paused_allocated_cpu,   static_cast<uint32_t>(8));
    MT_EXPECT_EQ(m.paused_allocated_memory_bytes,
                 static_cast<uint64_t>(1024) * 1024 * 1024);
}

// ---- metrics.rs :: aggregate_treats_pausing_and_snapshotting_as_running ----
MT_TEST(aggregate_pausing_and_snapshotting_count_as_running) {
    std::vector<SandboxMetadata> metas;
    metas.push_back(meta(SandboxState::Pausing,      2, 256));
    metas.push_back(meta(SandboxState::Snapshotting, 1, 128));

    OrchestratorMetrics m = aggregate(metas);
    MT_EXPECT_EQ(m.running_sandbox_count,  static_cast<uint32_t>(2));
    MT_EXPECT_EQ(m.starting_sandbox_count, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.allocated_cpu,          static_cast<uint32_t>(3));
    MT_EXPECT_EQ(m.allocated_memory_bytes,
                 static_cast<uint64_t>(256u + 128) * 1024 * 1024);
    MT_EXPECT_EQ(m.paused_sandbox_count,   static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.paused_allocated_cpu,   static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.paused_allocated_memory_bytes, static_cast<uint64_t>(0));
}

// ---- metrics.rs :: aggregate_paused_sums_independently_of_active_resources ----
MT_TEST(aggregate_paused_sums_independently) {
    std::vector<SandboxMetadata> metas;
    metas.push_back(meta(SandboxState::Paused,  4, 512));
    metas.push_back(meta(SandboxState::Paused,  2, 128));
    metas.push_back(meta(SandboxState::Running, 1, 64));

    OrchestratorMetrics m = aggregate(metas);
    MT_EXPECT_EQ(m.running_sandbox_count, static_cast<uint32_t>(1));
    MT_EXPECT_EQ(m.allocated_cpu,         static_cast<uint32_t>(1));
    MT_EXPECT_EQ(m.allocated_memory_bytes,
                 static_cast<uint64_t>(64) * 1024 * 1024);
    MT_EXPECT_EQ(m.paused_sandbox_count,  static_cast<uint32_t>(2));
    MT_EXPECT_EQ(m.paused_allocated_cpu,  static_cast<uint32_t>(4 + 2));
    MT_EXPECT_EQ(m.paused_allocated_memory_bytes,
                 static_cast<uint64_t>(512u + 128) * 1024 * 1024);
}

// ---- metrics.rs :: aggregate_empty_produces_default ----
MT_TEST(aggregate_empty_produces_default) {
    std::vector<SandboxMetadata> empty;
    OrchestratorMetrics m = aggregate(empty);
    MT_EXPECT_EQ(m.running_sandbox_count,  static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.starting_sandbox_count, static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.allocated_cpu,          static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.allocated_memory_bytes, static_cast<uint64_t>(0));
    MT_EXPECT_EQ(m.paused_sandbox_count,   static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.paused_allocated_cpu,   static_cast<uint32_t>(0));
    MT_EXPECT_EQ(m.paused_allocated_memory_bytes, static_cast<uint64_t>(0));
}

// ---- store/metadata.rs :: metadata_timeout_work ----
MT_TEST(metadata_timeout_work) {
    // Rust anchors the deadline on the `from` instant; here created_at_ms is
    // that anchor (Rust test passes `base` to the private `_set_timeout`).
    SandboxMetadata m;
    m.created_at_ms = 100 * 1000;
    m.SetTimeout(core::Optional<int64_t>(10 * 1000));

    MT_EXPECT_TRUE(m.expires_at_ms.has_value());
    MT_EXPECT_EQ(*m.expires_at_ms, static_cast<int64_t>(110 * 1000));
    MT_EXPECT_TRUE(!m.IsExpired(109 * 1000));
    MT_EXPECT_TRUE(m.IsExpired(110 * 1000));

    m.SetTimeout(core::Optional<int64_t>(core::nullopt));
    MT_EXPECT_TRUE(!m.timeout_ms.has_value());
    MT_EXPECT_TRUE(!m.expires_at_ms.has_value());
}

// ---- store/metadata.rs :: update_timeout_ensure_minimum_respects_longer ----
MT_TEST(update_timeout_ensure_minimum_respects_longer_existing) {
    SandboxMetadata m;
    m.created_at_ms = 100 * 1000;
    m.SetTimeout(core::Optional<int64_t>(900 * 1000));

    m.UpdateTimeout(NewTimeout::EnsureMinimum(300 * 1000));
    MT_EXPECT_TRUE(m.timeout_ms.has_value());
    MT_EXPECT_EQ(*m.timeout_ms, static_cast<int64_t>(900 * 1000));
    MT_EXPECT_EQ(*m.expires_at_ms, static_cast<int64_t>(100 * 1000 + 900 * 1000));
}

// ---- store/metadata.rs :: update_timeout_supports_set_use_existing_and_clear ----
MT_TEST(update_timeout_supports_set_use_existing_and_clear) {
    SandboxMetadata m;
    m.created_at_ms = 100 * 1000;
    m.SetTimeout(core::Optional<int64_t>(120 * 1000));

    // EnsureMinimum raises a shorter existing timeout.
    m.UpdateTimeout(NewTimeout::EnsureMinimum(300 * 1000));
    MT_EXPECT_EQ(*m.timeout_ms,    static_cast<int64_t>(300 * 1000));
    MT_EXPECT_EQ(*m.expires_at_ms, static_cast<int64_t>(400 * 1000));

    // UseExisting leaves it untouched.
    m.UpdateTimeout(NewTimeout::UseExisting());
    MT_EXPECT_EQ(*m.timeout_ms,    static_cast<int64_t>(300 * 1000));
    MT_EXPECT_EQ(*m.expires_at_ms, static_cast<int64_t>(400 * 1000));

    // Set overrides unconditionally, even downward.
    m.UpdateTimeout(NewTimeout::Set(45 * 1000));
    MT_EXPECT_EQ(*m.timeout_ms,    static_cast<int64_t>(45 * 1000));
    MT_EXPECT_EQ(*m.expires_at_ms, static_cast<int64_t>(145 * 1000));

    // None clears both.
    m.UpdateTimeout(NewTimeout::None());
    MT_EXPECT_TRUE(!m.timeout_ms.has_value());
    MT_EXPECT_TRUE(!m.expires_at_ms.has_value());
}

// ---- store/in_memory.rs coverage ----
MT_TEST(store_add_get_update_remove) {
    InMemoryMetadataStore s;
    SandboxMetadata m = meta(SandboxState::Running, 2, 256);
    core::SandboxId id = m.id;

    MT_EXPECT_TRUE(s.Add(m).ok());

    // Rust: duplicate add -> StoreError::SandboxAlreadyExists.
    core::Expected<core::Unit, StoreError> dup = s.Add(m);
    MT_EXPECT_TRUE(!dup.ok());
    MT_EXPECT_TRUE(dup.error().kind == StoreError::Kind::SandboxAlreadyExists);

    // Rust `get` returns Result<Option<_>> — found.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> g = s.Get(id);
    MT_EXPECT_TRUE(g.ok());
    MT_EXPECT_TRUE(g.value().has_value());
    MT_EXPECT_EQ(g.value()->resources.cpu_count, static_cast<uint32_t>(2));

    m.resources.cpu_count = 8;
    MT_EXPECT_TRUE(s.Update(m).ok());
    MT_EXPECT_EQ(s.Get(id).value()->resources.cpu_count, static_cast<uint32_t>(8));

    // Rust `remove` returns the removed record.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> rm = s.Remove(id);
    MT_EXPECT_TRUE(rm.ok());
    MT_EXPECT_TRUE(rm.value().has_value());

    // Rust: get on a missing id is Ok(None), NOT an error.
    core::Expected<core::Optional<SandboxMetadata>, StoreError> gone = s.Get(id);
    MT_EXPECT_TRUE(gone.ok());
    MT_EXPECT_TRUE(!gone.value().has_value());
}

MT_TEST(store_update_state_if_state) {
    InMemoryMetadataStore s;
    SandboxMetadata m = meta(SandboxState::Creating, 1, 128);
    core::SandboxId id = m.id;
    MT_EXPECT_TRUE(s.Add(m).ok());

    std::vector<SandboxState> expected;
    expected.push_back(SandboxState::Creating);

    // Rust returns the PREVIOUS state on success.
    core::Expected<SandboxState, StoreError> prev =
        s.UpdateStateIfState(id, SandboxState::Running, expected);
    MT_EXPECT_TRUE(prev.ok());
    MT_EXPECT_TRUE(prev.value() == SandboxState::Creating);
    MT_EXPECT_TRUE(s.Get(id).value()->state == SandboxState::Running);

    // State no longer matches -> StateConflict.
    core::Expected<SandboxState, StoreError> conflict =
        s.UpdateStateIfState(id, SandboxState::Paused, expected);
    MT_EXPECT_TRUE(!conflict.ok());
    MT_EXPECT_TRUE(conflict.error().kind == StoreError::Kind::StateConflict);
}

MT_TEST(store_list_filtered_and_expired) {
    InMemoryMetadataStore s;
    SandboxMetadata running = meta(SandboxState::Running, 1, 128);
    SandboxMetadata paused  = meta(SandboxState::Paused,  2, 256);
    paused.created_at_ms = 1000;
    paused.SetTimeout(core::Optional<int64_t>(500));  // expires at 1500
    MT_EXPECT_TRUE(s.Add(running).ok());
    MT_EXPECT_TRUE(s.Add(paused).ok());

    MT_EXPECT_EQ(s.List().value().size(), static_cast<std::size_t>(2));
    MT_EXPECT_EQ(s.ListIds().value().size(), static_cast<std::size_t>(2));

    // states whitelist
    SandboxListFilter only_running;
    std::vector<SandboxState> want;
    want.push_back(SandboxState::Running);
    only_running.states = want;
    MT_EXPECT_EQ(s.ListFiltered(only_running).value().size(),
                 static_cast<std::size_t>(1));

    // excluded_states blacklist
    SandboxListFilter not_paused;
    std::vector<SandboxState> skip;
    skip.push_back(SandboxState::Paused);
    not_paused.excluded_states = skip;
    MT_EXPECT_EQ(s.ListFiltered(not_paused).value().size(),
                 static_cast<std::size_t>(1));

    // expiry
    MT_EXPECT_EQ(s.ListExpired(1499).value().size(), static_cast<std::size_t>(0));
    MT_EXPECT_EQ(s.ListExpired(1500).value().size(), static_cast<std::size_t>(1));
}

MT_MAIN
